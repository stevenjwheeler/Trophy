#!/usr/bin/env python3
"""Bounded, static ELF dependency/version checks; never execute inspected ELFs."""
import argparse
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile

MAX_OUTPUT = 8 * 1024 * 1024
MAX_OBJECTS = 64


class CheckError(Exception):
    pass


def run_tool(argv):
    # File-backed output avoids unbounded subprocess.PIPE allocation.
    with tempfile.TemporaryFile() as output:
        try:
            env = {k: v for k, v in os.environ.items()
                   if k not in ("LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH")}
            result = subprocess.run(argv, stdout=output, stderr=output,
                                    timeout=15, env={**env, "LC_ALL": "C"},
                                    preexec_fn=lambda: resource.setrlimit(
                                        resource.RLIMIT_FSIZE, (MAX_OUTPUT, MAX_OUTPUT)))
        except (OSError, subprocess.TimeoutExpired) as error:
            raise CheckError(f"{argv[0]}: {error}") from error
        size = output.tell()
        if size > MAX_OUTPUT:
            raise CheckError(f"{argv[0]} output exceeds {MAX_OUTPUT} bytes")
        output.seek(0)
        text = output.read().decode("utf-8", errors="replace")
        if result.returncode:
            raise CheckError(f"{argv[0]} failed ({result.returncode}): {text[:1000]}")
        return text


def parse_elf(text):
    def field(name):
        match = re.search(r"^\s*" + name + r":\s*(\S.*)$", text, re.M)
        if not match:
            raise CheckError(f"missing ELF {name}")
        return match.group(1).strip()
    elf_class, machine = field("Class"), field("Machine")
    if elf_class not in ("ELF32", "ELF64"):
        raise CheckError(f"invalid ELF class: {elf_class}")
    needed = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", text)
    search_paths = re.findall(r"\((?:RPATH|RUNPATH)\).*\[([^\]]*)\]", text)
    requirements, definitions, exports, undefined = {}, set(), set(), []
    version_providers = {}
    section, provider = None, None
    for line in text.splitlines():
        if line.startswith("Version needs section"):
            section, provider = "needs", None
        elif line.startswith("Version definition section"):
            section = "definitions"
        elif line.startswith("Version symbols section"):
            section = None
        file_match = re.search(r"\bFile:\s*(\S+)\s+Cnt:", line)
        if section == "needs" and file_match:
            provider = file_match.group(1)
            requirements.setdefault(provider, set())
        name = re.search(r"\bName:\s*(\S+)", line)
        if name and section == "needs":
            if provider is None:
                raise CheckError("version requirement without provider")
            requirements[provider].add(name.group(1))
            version_index = re.search(r"\bVersion:\s*(\d+)", line)
            if not version_index:
                raise CheckError("version requirement without index")
            version_providers[int(version_index.group(1))] = provider
        elif name and section == "definitions":
            definitions.add(name.group(1))
        # --dyn-syms: Num Value Size Type Bind Vis Ndx Name [version index]
        symbol = re.match(r"\s*\d+:\s+\S+\s+\S+\s+\S+\s+(GLOBAL|WEAK|UNIQUE)\s+"
                          r"(\S+)\s+(\S+)\s+(\S+)", line)
        if symbol:
            bind, visibility, index, name = symbol.groups()
            if index == "UND":
                if bind != "WEAK" and "@" in name:
                    version_index = re.search(r"\((\d+)\)\s*$", line)
                    if not version_index:
                        raise CheckError("undefined versioned symbol without index")
                    undefined.append((*name.split("@", 1), int(version_index.group(1))))
            elif visibility in ("DEFAULT", "PROTECTED"):
                exports.add(name.replace("@@", "@"))
    if any("/" in name or name in (".", "..") for name in needed):
        raise CheckError("path-valued DT_NEEDED is unsupported")
    for _, _, index in undefined:
        if index not in version_providers:
            raise CheckError(f"undefined symbol references unknown version index {index}")
    return {"class": elf_class, "machine": machine, "needed": needed,
            "search_paths": search_paths,
            "requirements": requirements, "definitions": definitions,
            "exports": exports,
            "undefined": [(symbol, version, version_providers[index])
                          for symbol, version, index in undefined]}


def inspect(path):
    return parse_elf(run_tool(["readelf", "--wide", "--file-header", "--dynamic",
                               "--version-info", "--dyn-syms", str(path)]))


def parse_ldconfig(text):
    result = {}
    for line in text.splitlines():
        match = re.match(r"\s*(\S+)\s+\([^)]*\)\s+=>\s+(/\S+)\s*$", line)
        if match:
            result.setdefault(match.group(1), []).append(Path(match.group(2)))
    if not result:
        raise CheckError("ldconfig returned no materialized providers")
    return result


def sysroot_providers(root):
    root = root.resolve()
    result, count = {}, 0
    for prefix in ("lib", "lib64", "lib32", "usr/lib", "usr/lib64", "usr/lib32"):
        start = root / prefix
        if not start.exists():
            continue
        if not start.resolve().is_relative_to(root):
            raise CheckError(f"sysroot directory escapes root: {start}")
        for directory, dirs, files in os.walk(start, followlinks=False):
            if len(Path(directory).relative_to(start).parts) >= 4:
                dirs[:] = []
            count += len(dirs) + len(files)
            if count > 20000:
                raise CheckError("sysroot provider search exceeds 20000 entries")
            for name in files:
                if ".so" not in name:
                    continue
                path = Path(directory) / name
                # Absolute sysroot symlinks must not accidentally select host libs.
                for _ in range(16):
                    if not path.is_symlink():
                        break
                    target = os.readlink(path)
                    path = root / target.lstrip("/") if target.startswith("/") else path.parent / target
                else:
                    raise CheckError(f"symlink chain too long: {path}")
                path = path.resolve()
                if not path.is_relative_to(root):
                    raise CheckError(f"sysroot provider escapes root: {path}")
                if path.is_file():
                    result.setdefault(name, []).append(path)
    if not result:
        raise CheckError("no materialized sysroot libraries (mtree metadata is not a sysroot)")
    return result


def check(reporter, providers):
    cache, selected, errors = {}, {}, []
    def load(path):
        path = Path(path).resolve()
        if path not in cache:
            if len(cache) >= MAX_OBJECTS:
                raise CheckError(f"inspection exceeds {MAX_OBJECTS} objects")
            cache[path] = inspect(path)
        return cache[path]
    original = load(reporter)
    queue = [Path(reporter).resolve()]
    visited = set()
    while queue:
        path = queue.pop(0)
        if path in visited:
            continue
        visited.add(path)
        obj = load(path)
        for name in dict.fromkeys(obj["needed"] + list(obj["requirements"])):
            if name not in selected:
                compatible = []
                for candidate in providers.get(name, []):
                    candidate_obj = load(candidate)
                    if (candidate_obj["class"], candidate_obj["machine"]) == (original["class"], original["machine"]):
                        compatible.append(candidate.resolve())
                compatible = list(dict.fromkeys(compatible))
                if len(compatible) != 1:
                    errors.append(f"{name}: expected one architecture-matching provider, found {len(compatible)}")
                    continue
                selected[name] = compatible[0]
                queue.append(compatible[0])
            provider = load(selected[name])
            missing = obj["requirements"].get(name, set()) - provider["definitions"]
            if missing:
                errors.append(f"{path.name} -> {name}: missing versions {', '.join(sorted(missing))}")
            for symbol, version, symbol_provider in obj["undefined"]:
                if symbol_provider == name and f"{symbol}@{version}" not in provider["exports"]:
                    errors.append(f"{path.name} -> {name}: missing symbol {symbol}@{version}")
    return {"reporter": str(Path(reporter).resolve()), "class": original["class"],
            "machine": original["machine"],
            "needed": original["needed"], "search_paths_not_applied": original["search_paths"],
            "requirements": {k: sorted(v) for k, v in original["requirements"].items()},
            "providers": {k: str(v) for k, v in sorted(selected.items())},
            "inspected_objects": len(cache), "errors": errors,
            "status": "static-check-pass" if not errors else "static-check-fail"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reporters", nargs="+", type=Path)
    parser.add_argument("--sysroot", type=Path, help="materialized ELF sysroot; never falls back to host")
    args = parser.parse_args()
    try:
        providers = sysroot_providers(args.sysroot) if args.sysroot else parse_ldconfig(run_tool(["ldconfig", "-p"]))
        reports = [check(path, providers) for path in args.reporters]
    except (CheckError, OSError) as error:
        print(json.dumps({"status": "inconclusive", "error": str(error)}, indent=2))
        return 2
    print(json.dumps({"scope": "static architecture, dependency closure, version definitions and strong versioned symbols; not loader execution; RPATH/RUNPATH and environment overrides not applied",
                      "environment": str(args.sysroot) if args.sysroot else "host ldconfig providers",
                      "reports": reports}, indent=2))
    return int(any(report["errors"] for report in reports))


if __name__ == "__main__":
    raise SystemExit(main())
