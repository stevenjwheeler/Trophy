#!/usr/bin/env python3
"""Static ABI matrix plus opt-in, graphics-free loading of trusted reporters.

Explicit sysroots never fall back to host providers. This does not launch Steam,
pressure-vessel, a game, or a graphics driver, or change desktop enrollment.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

import check_reporter_abi as abi

PROBE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 3; }
  const char *symbols_gl[] = {"eglSwapBuffers", "glXSwapBuffers", "SDL_GL_SwapWindow", 0};
  const char *symbols_vk[] = {"vkNegotiateLoaderLayerInterfaceVersion", 0};
  const char **symbols = strcmp(argv[2], "gl") == 0 ? symbols_gl : symbols_vk;
  for (; *symbols; ++symbols) if (!dlsym(lib, *symbols)) return 4;
  struct link_map *map = 0;
  if (dlinfo(lib, RTLD_DI_LINKMAP, &map)) return 6;
  while (map->l_prev) map = map->l_prev;
  for (; map; map = map->l_next)
    if (map->l_name && map->l_name[0] && strncmp(map->l_name, "linux-vdso", 10) &&
        strcmp(map->l_name, "linux-gate.so.1"))
      printf("OBJECT:%s\n", map->l_name);
  if (dlclose(lib)) return 5;
  puts("trusted reporter loaded/resolved/unloaded without graphics calls");
  return 0;
}
'''


def clean_environment(home):
    # No Steam, graphics, loader or reporter overrides inherited from the session.
    return {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C",
            "HOME": str(home), "DISABLE_TROPHY_DETECTION": "1"}


def evaluate(name, root, reporters, probes, execute):
    result = {"name": name, "sysroot": str(root) if root else None, "reporters": []}
    try:
        providers = abi.sysroot_providers(root) if root else abi.parse_ldconfig(abi.run_tool(["ldconfig", "-p"]))
    except (abi.CheckError, OSError) as error:
        result.update(status="inconclusive", error=str(error))
        return result
    for path in reporters:
        row = {"reporter": str(path), "fixture": {"status": "not-requested"}}
        result["reporters"].append(row)
        try:
            row["abi"] = abi.check(path, providers)
            if not execute:
                continue
            bits = 32 if row["abi"]["class"] == "ELF32" else 64
            probe = probes.get(bits)
            if not probe:
                row["fixture"] = {"status": "unavailable", "reason": "matching compiler probe unavailable"}
                continue
            bootstrap = abi.check(probe, providers)
            if row["abi"]["errors"] or bootstrap["errors"]:
                row["fixture"] = {"status": "not-run-abi-unsatisfied", "probe_abi": bootstrap}
                continue
            # Only dependencies already proven against this explicit provider
            # set enter the loader path; never append host dirs to a sysroot.
            selected = {**bootstrap["providers"], **row["abi"]["providers"]}
            loaders = [value for key, value in selected.items() if key.startswith(("ld-linux", "ld64"))]
            if len(set(loaders)) != 1:
                row["fixture"] = {"status": "unavailable", "reason": "no unique checked runtime loader"}
                continue
            directories = sorted({str(Path(value).parent) for value in selected.values()})
            helper = "vulkan"
            command = [loaders[0], "--inhibit-cache", "--library-path", ":".join(directories), str(probe), str(path), helper]
            run = subprocess.run(command, env=clean_environment(probe.parent), capture_output=True,
                                 text=True, timeout=20)
            row["fixture"] = {"status": "pass" if run.returncode == 0 else "fail",
                              "command": command, "returncode": run.returncode,
                              "stdout": run.stdout[:4096], "stderr": run.stderr[:4096]}
            loaded = [line.removeprefix("OBJECT:") for line in run.stdout.splitlines()
                      if line.startswith("OBJECT:")]
            allowed = {Path(value).resolve() for value in selected.values()} | {path.resolve(), probe.resolve()}
            foreign = [value for value in loaded if Path(value).resolve() not in allowed]
            row["fixture"]["loaded_objects"] = loaded
            if run.returncode == 0 and (not loaded or foreign):
                row["fixture"].update(status="fail", unexpected_objects=foreign,
                                      reason="loaded objects differ from explicit checked providers")
        except (abi.CheckError, OSError, subprocess.TimeoutExpired) as error:
            row["error"] = str(error)
    result["status"] = "checked"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reporters", nargs="+", type=Path, help="trusted reporter build outputs")
    parser.add_argument("--runtime", action="append", default=[], metavar="NAME=SYSROOT")
    parser.add_argument("--execute-trusted-fixtures", action="store_true",
                        help="compile matching probes and dlopen trusted reporters with reporting disabled")
    parser.add_argument("--require-host-pass", action="store_true",
                        help="fail CI if any requested host ABI check or requested fixture does not pass")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    runtimes = [("host", None)]
    for value in args.runtime:
        name, separator, path = value.partition("=")
        if not separator or not name or not path:
            parser.error("--runtime requires NAME=SYSROOT")
        runtimes.append((name, Path(path).absolute()))
    if len(runtimes) > 16 or len(args.reporters) > 16:
        parser.error("matrix exceeds 16 runtimes/reporters")
    base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="reporter-runtime-probes-", dir=base) as directory:
        workspace = Path(directory)
        source = workspace / "probe.c"
        source.write_text(PROBE)
        probes, compiler = {}, {}
        if args.execute_trusted_fixtures:
            for bits in (32, 64):
                target = workspace / f"probe{bits}"
                try:
                    run = subprocess.run(["gcc", f"-m{bits}", str(source), "-ldl", "-o", str(target)],
                                         env=clean_environment(workspace), capture_output=True, text=True, timeout=60)
                    compiler[str(bits)] = {"returncode": run.returncode, "stderr": run.stderr[:4096]}
                    if run.returncode == 0:
                        probes[bits] = target
                except (OSError, subprocess.TimeoutExpired) as error:
                    compiler[str(bits)] = {"error": str(error)}
        report = {"scope": "explicit static providers and disabled graphics-free load/unload only; not Steam provider selection, graphics or game compatibility",
                  "compiler": compiler,
                  "runtimes": [evaluate(name, root, [p.absolute() for p in args.reporters], probes,
                                        args.execute_trusted_fixtures) for name, root in runtimes]}
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(args.output)
        # Compatibility negatives remain evidence, not universal CI failures.
        # Invocation/tool errors are distinct from a known incompatible ABI.
        failed = any("error" in row or row["fixture"]["status"] == "fail"
                     for runtime in report["runtimes"] for row in runtime["reporters"])
        if args.require_host_pass:
            host = report["runtimes"][0]
            failed |= host["status"] != "checked" or any(
                row.get("abi", {}).get("status") != "static-check-pass" or
                (args.execute_trusted_fixtures and row["fixture"]["status"] != "pass")
                for row in host["reporters"])
        return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
