"""Check required ELF symbol versions against the Ubuntu 24.04 AppImage baseline.

Only inspect trusted AppImages: extraction executes the AppImage runtime.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


LIMITS = {"GLIBC": "2.39", "GLIBCXX": "3.4.32", "CXXABI": "1.3.14"}


def required_versions(output):
    """Ignore exported definitions: only .gnu.version_r constrains the host."""
    reading = False
    versions = set()
    for line in output.splitlines():
        if line.startswith("Version needs section"):
            reading = True
        elif line.startswith(("Version definition section", "Version symbols section")):
            reading = False
        if reading:
            match = re.search(r"\bName:\s*((?:GLIBCXX|GLIBC|CXXABI)_[\w.]+)", line)
            if match:
                versions.add(match.group(1))
    return versions


def version_tuple(version):
    return tuple(map(int, version.split(".")))


def violations(versions, limits=LIMITS):
    errors = []
    for version in sorted(versions):
        # DT_RELR support was introduced in glibc 2.36; it is a nonnumeric
        # version need, even when no dynamic symbol references that version.
        if version == "GLIBC_ABI_DT_RELR" and version_tuple(limits["GLIBC"]) >= (2, 36):
            continue
        namespace, number = version.split("_", 1)
        if not re.fullmatch(r"\d+(?:\.\d+)+", number):
            errors.append("Unsupported symbol-version requirement: " + version)
        elif version_tuple(number) > version_tuple(limits[namespace]):
            errors.append(f"{version} exceeds {namespace}_{limits[namespace]}")
    return errors


def scan(directory, extra_files=()):
    files = list(directory.rglob("*")) + list(extra_files)
    inspected = 0
    requirements = set()
    offenders = []
    for path in files:
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        inspected += 1
        output = subprocess.check_output(["readelf", "--version-info", "--wide", str(path)],
                                         text=True, stderr=subprocess.PIPE, env=dict(os.environ, LC_ALL="C"))
        needed = required_versions(output)
        requirements.update(needed)
        errors = violations(needed)
        if errors:
            name = str(path.relative_to(directory)) if path.is_relative_to(directory) else path.name
            offenders.append({"file": name, "errors": errors})
    if not inspected or not any(version.startswith("GLIBC_2.") for version in requirements):
        raise RuntimeError("No inspectable glibc-linked ELF payload found")
    maxima = {}
    for namespace in LIMITS:
        numbers = [version[len(namespace) + 1:] for version in requirements
                   if re.fullmatch(namespace + r"_\d+(?:\.\d+)+", version)]
        if numbers:
            maxima[namespace] = max(numbers, key=version_tuple)
    return {"baseline": "Ubuntu 24.04 x86_64", "limits": LIMITS, "elf_files": inspected,
            "maximum_required": maxima, "offenders": offenders, "passed": not offenders}


def check(path):
    if path.is_dir():
        return scan(path)
    if not path.is_file() or path.is_symlink():
        raise RuntimeError("Expected a trusted AppDir or regular AppImage")
    base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="trophy-abi-", dir=base) as directory:
        env = os.environ.copy()
        for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "APPDIR", "APPIMAGE", "ARGV0", "OWD",
                     "APPIMAGE_EXTRACT_AND_RUN"):
            env.pop(name, None)
        result = subprocess.run([str(path.absolute()), "--appimage-extract"], cwd=directory,
                                env=env, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError("AppImage extraction failed: " + result.stderr[-2000:])
        payload = Path(directory) / "squashfs-root"
        if not payload.is_dir():
            raise RuntimeError("AppImage did not extract a payload")
        return scan(payload, [path])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = check(args.payload)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report = {"passed": False, "error": str(error), "limits": LIMITS}
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
