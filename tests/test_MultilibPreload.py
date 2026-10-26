"""Real environment.d + ELF32/ELF64 loader selection, no graphics or socket use."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


def check(qt_library, qt_includes):
    generator = Path("/usr/lib/systemd/user-environment-generators/30-systemd-environment-d-generator")
    if platform.machine() not in ("x86_64", "amd64") or not generator.is_file() or not all(
            shutil.which(name) for name in ("gcc", "g++")):
        print("SKIP: requires x86 Linux, systemd environment generator and GNU multilib tools")
        return 77
    base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="trophy-preload-test-", dir=base) as directory:
        root = Path(directory)
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8",
               "HOME": str(root / "home"), "XDG_CONFIG_HOME": str(root / "config"),
               "XDG_DATA_HOME": str(root / "data"), "XDG_CONFIG_DIRS": str(root / "empty"),
               "PLATFORM": "hostile-value", "SYSTEMD_LOG_LEVEL": "err"}
        source = root / "fixture.c"
        source.write_text("int trophy_preload_fixture_bits(void) { return sizeof(void*) * 8; }\n")
        runner = root / "runner.c"
        runner.write_text('#include <dlfcn.h>\n#include <stdio.h>\nint main(void) { '
                          'int (*f)(void)=dlsym(RTLD_DEFAULT,"trophy_preload_fixture_bits"); '
                          'if(!f) return 3; printf("%d\\n",f()); return 0; }\n')
        for bits in (32, 64):
            result = subprocess.run(["gcc", f"-m{bits}", "-shared", "-fPIC", str(source),
                                     "-o", str(root / f"fixture{bits}.so")], env=env,
                                    capture_output=True, text=True, timeout=60)
            if result.returncode and bits == 32:
                print("SKIP: i386 compiler/runtime development files unavailable")
                return 77
            if result.returncode:
                raise RuntimeError(result.stderr)
            subprocess.run(["gcc", f"-m{bits}", str(runner), "-ldl", "-o", str(root / f"run{bits}")],
                           env=env, capture_output=True, check=True, timeout=60)
        adapter = root / "enroll.cpp"
        adapter.write_text('#include <QCoreApplication>\n#include "detection/GlShimEnrollment.h"\n'
                           'int main(int argc,char**argv) { QCoreApplication app(argc,argv); '
                           'bool on=argc>1 && argv[1][0]==\'1\'; '
                           'return TrophyBackend::GlShimEnrollment::setEnrolled(on) && '
                           'TrophyBackend::GlShimEnrollment::isEnrolled()==on ? 0:1; }\n')
        repo = Path(__file__).parents[1]
        command = ["g++", "-std=c++20", "-fPIC", "-pthread", str(adapter),
                   str(repo / "src/detection/GlShimEnrollment.cpp"), "-I" + str(repo / "src")]
        command += ["-I" + value for value in qt_includes]
        command += [str(qt_library), "-Wl,-rpath," + str(qt_library.parent), "-o", str(root / "enroll")]
        subprocess.run(command, env=env, capture_output=True, check=True, timeout=90)
        enrollment_env = {**env, "TROPHY_GL_SHIM_LIB": str(root / "fixture64.so"),
                          "TROPHY_GL_SHIM_LIB32": str(root / "fixture32.so")}
        subprocess.run([str(root / "enroll"), "1"], env=enrollment_env, check=True, timeout=30)
        config = root / "config/environment.d/trophy-gl-shim.conf"
        text = config.read_text()
        token = next(line[11:] for line in text.splitlines() if line.startswith("LD_PRELOAD="))
        if "/$$PLATFORM/" not in token:
            raise RuntimeError("Global enrollment did not select an architecture pair")
        generation = subprocess.run([str(generator)], env=env, capture_output=True,
                                    text=True, check=True, timeout=30)
        assignment = next(line for line in generation.stdout.splitlines() if line.startswith("LD_PRELOAD="))
        value = token.replace("$$PLATFORM", "$PLATFORM")
        # Validate the exact escaped assignment without eval/sourcing arbitrary
        # generator output. $PLATFORM must survive even when an env var exists.
        expected = 'LD_PRELOAD="' + value.replace('$', '\\$') + '"'
        if assignment != expected:
            raise RuntimeError(f"Generator expanded/changed loader token: {assignment!r}")
        for bits in (32, 64):
            result = subprocess.run([str(root / f"run{bits}")], env={**env, "LD_PRELOAD": value},
                                    capture_output=True, text=True, timeout=30)
            if result.returncode or result.stdout.strip() != str(bits) or result.stderr:
                raise RuntimeError(f"ELF{bits} preload selection failed: {result}")
        # Negative control: the former single absolute ELF64 path cannot serve
        # the same i386 runner. Don't mistake an ignored preload for success.
        negative = subprocess.run([str(root / "run32")], env={**env, "LD_PRELOAD": str(root / "fixture64.so")},
                                  capture_output=True, text=True, timeout=30)
        if negative.returncode == 0 or "ELFCLASS64" not in negative.stderr:
            raise RuntimeError("Wrong-architecture negative control did not fail as expected")
        subprocess.run([str(root / "enroll"), "0"], env=enrollment_env, check=True, timeout=30)
        if config.exists():
            raise RuntimeError("Global opt-out did not remove managed enrollment")
        print("Production enrollment + systemd + ELF32/ELF64 selection/opt-out passed; wrong-ELF negative control failed")
        return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-library", type=Path, required=True)
    parser.add_argument("--qt-include", action="append", default=[])
    args = parser.parse_args()
    raise SystemExit(check(args.qt_library, args.qt_include))
