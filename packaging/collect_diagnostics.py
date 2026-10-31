"""Collect Trophy diagnostics: run isolated tests, gather system/Trophy info, package a report.

This replaces the former diagnostic bundle. It does not require a special diagnostic
AppImage or Docker. Point it at a normal Trophy AppImage/build if you also want
the bundled source-free test executables; otherwise it collects system
information, the Trophy log (if present) and installed-binary details.

Interactive wizard (default when run in a terminal):
    python3 packaging/collect_diagnostics.py

Non-interactive:
    python3 packaging/collect_diagnostics.py --appimage /path/to/Trophy.AppImage
    python3 packaging/collect_diagnostics.py --build /path/to/build --output report.tar.gz
    python3 packaging/collect_diagnostics.py --noninteractive --appimage /path/to/Trophy.AppImage
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path


TESTS = (
    "test_EncoderTest", "test_DmaBufCudaImport", "test_VaapiVideoProcessor",
    "test_EglDisplayLease", "test_Muxer", "test_EncoderFactory", "test_AudioEncoder",
    "test_AudioDriftCorrector", "test_FramePipeline", "test_PipeWireVideoFormat",
    "test_PipeWireBufferLease", "test_PipeWireSyncWait", "test_AudioMixer",
)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run_owned(command, env, log, timeout):
    with log.open("w") as stream:
        process = subprocess.Popen(command, env=env, stdout=stream, stderr=stream,
                                   start_new_session=True)
        try:
            return process.wait(timeout=timeout)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def test_environment(work):
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("QT_", "QML_", "TROPHY_", "PIPEWIRE_", "WIREPLUMBER_", "GTEST_")) or key in {
            "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "APPDIR", "APPIMAGE", "ARGV0", "OWD",
            "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID",
            "LIBGL_ALWAYS_SOFTWARE", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION"
        }:
            env.pop(key, None)
    for key, name in (("HOME", "test-home"), ("XDG_CONFIG_HOME", "test-config"),
                      ("XDG_DATA_HOME", "test-data"), ("XDG_CACHE_HOME", "test-cache"),
                      ("XDG_STATE_HOME", "test-state"), ("XDG_RUNTIME_DIR", "test-runtime"), ("TMPDIR", "test-tmp")):
        directory = work / name
        directory.mkdir(mode=0o700, exist_ok=True)
        env[key] = str(directory)
    env.update(DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(work / "no-session-bus"),
               DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(work / "no-system-bus"),
               PIPEWIRE_REMOTE="trophy-no-host-daemon", QT_QPA_PLATFORM="offscreen",
               APPIMAGE_EXTRACT_AND_RUN="1")
    return env


def case_status(case):
    if case.find("failure") is not None or case.find("error") is not None:
        return "failed"
    if case.find("skipped") is not None or case.get("status") == "notrun":
        return "skipped"
    return "passed"


def gpu_inventory(directory=Path("/sys/class/drm")):
    cards = []
    for card in sorted(directory.glob("card*")):
        if not re.fullmatch(r"card\d+", card.name):
            continue
        device = card / "device"
        info = {"card": card.name}
        for key in ("vendor", "device"):
            try:
                info[key] = (device / key).read_text()[:80].strip()
            except OSError:
                pass
        try:
            info["driver"] = (device / "driver").resolve(strict=True).name
        except OSError:
            pass
        try:
            info["module_version"] = (device / "driver/module/version").read_text()[:80].strip()
        except OSError:
            pass
        cards.append(info)
    return cards


def system_info():
    info = {
        "kernel": platform.release(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "libc": platform.libc_ver(),
        "desktop": os.environ.get("XDG_CURRENT_DESKTOP", "unknown"),
        "session_type": os.environ.get("XDG_SESSION_TYPE", "unknown"),
        "gpus": gpu_inventory(),
    }
    try:
        info["os_release"] = Path("/etc/os-release").read_text()[:2000]
    except OSError:
        pass
    trophy_paths = {}
    for candidate in ("~/.local/bin/trophy", "/usr/bin/trophy", "/usr/local/bin/trophy"):
        path = Path(candidate).expanduser()
        if path.is_file():
            trophy_paths[str(path)] = "present"
    info["trophy_binaries"] = trophy_paths
    return info


def find_trophy_log():
    candidates = []
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        candidates.append(Path(xdg) / "trophy" / "trophy.log")
    candidates.append(Path.home() / ".config" / "trophy" / "trophy.log")
    xdg_data = os.environ.get("XDG_DATA_HOME")
    if xdg_data:
        candidates.append(Path(xdg_data) / "trophy" / "trophy.log")
    candidates.append(Path.home() / ".local" / "share" / "trophy" / "trophy.log")
    for c in candidates:
        if c.is_file() and not c.is_symlink():
            return c
    return None


def redact(text, home, work):
    for value, replacement in ((str(work), "<TEST-DIRECTORY>"), (str(home), "<HOME>")):
        text = text.replace(value, replacement)
    return "\n".join("[REDACTED sensitive line]" if re.search(
        r"token|authorization|password|secret|cookie|bearer", line, re.I) else line
        for line in text.splitlines()) + "\n"


def run_embedded_tests(appimage, work, results):
    env = test_environment(work)
    image = str(Path(appimage).resolve())
    inventory = subprocess.run([image, "--trophy-test-inventory"], env=env,
                               capture_output=True, text=True, timeout=60)
    if inventory.returncode != 0:
        raise RuntimeError("This AppImage does not contain the diagnostic test inventory "
                           "(production AppImages do not include tests). "
                           "Use a diagnostic build or a build directory instead.")
    try:
        metadata = json.loads(inventory.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("Could not parse test inventory: " + str(error))
    test_list = metadata.get("tests", [])
    if metadata.get("schema") != 2 or test_list != list(TESTS):
        raise RuntimeError("This AppImage has an unsupported/older test inventory.")
    combined = ET.Element("testsuites")
    runs = []
    for name in test_list:
        print("Running", name, flush=True)
        xml = results / (name + ".xml")
        try:
            code = run_owned([image, "--trophy-test", name, "--gtest_output=xml:" + str(xml)],
                             env, results / (name + ".log"), 300)
            document = ET.parse(xml).getroot()
            if not list(document.iter("testcase")):
                raise RuntimeError("Empty test report")
            if document.tag == "testsuites":
                combined.extend(list(document))
            else:
                combined.append(document)
            runs.append({"binary": name, "exit": code, "status": "finished"})
        except (OSError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
            runs.append({"binary": name, "status": "incomplete", "reason": str(error)})
    ET.ElementTree(combined).write(results / "tests.xml", encoding="unicode")
    (results / "test-runs.json").write_text(json.dumps(runs, indent=2))
    cases = list(combined.iter("testcase"))
    failed = sum(case_status(case) == "failed" for case in cases)
    skipped = sum(case_status(case) == "skipped" for case in cases)
    return {"status": "finished", "total": len(cases), "failed": failed, "skipped": skipped,
            "passed": len(cases) - failed - skipped, "runs": runs, "mode": "embedded"}


def run_ctest(build, work, results):
    env = test_environment(work)
    build = Path(build).resolve()
    log = results / "ctest.log"
    with log.open("w") as stream:
        process = subprocess.Popen(["ctest", "--test-dir", str(build), "--output-on-failure"],
                                   env=env, stdout=stream, stderr=stream, start_new_session=True)
        try:
            code = process.wait(timeout=3600)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait()
    # Copy ctest's own report if present
    last = build / "Testing" / "Temporary" / "LastTest.log"
    if last.is_file():
        shutil.copyfile(last, results / "ctest-last.log")
    return {"status": "finished" if code == 0 else "failed", "exit": code, "mode": "ctest"}


def run_tests(appimage, build, work, results):
    summaries = []
    if appimage:
        try:
            summaries.append(run_embedded_tests(appimage, work, results))
        except (OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
            summaries.append({"status": "blocked_or_incomplete", "reason": str(error), "mode": "embedded"})
    if build:
        try:
            summaries.append(run_ctest(build, work, results))
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            summaries.append({"status": "blocked_or_incomplete", "reason": str(error), "mode": "ctest"})
    if not summaries:
        return {"status": "not_requested"}
    # Merge into a single summary for compatibility
    if len(summaries) == 1:
        return summaries[0]
    return {"status": "finished", "modes": summaries}


def package_report(results, output, home, work):
    allowed = {"summary.json", "feedback.txt", "tests.xml", "test-runs.json",
               "system-info.json", "trophy.log", "ctest.log", "ctest-last.log"}
    allowed.update(name + suffix for name in TESTS for suffix in (".xml", ".log"))
    base = "/tmp/trophy-diag" if Path("/tmp/trophy-diag").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="trophy-report-", dir=base) as directory:
        stage = Path(directory)
        for name in sorted(allowed):
            path = results / name
            if path.is_file() and not path.is_symlink():
                with path.open("rb") as stream:
                    if path.stat().st_size > 8 * 1024**2:
                        stream.seek(-8 * 1024**2, 2)
                        prefix = "[Earlier content omitted: report file size limit]\n"
                    else:
                        prefix = ""
                    text = stream.read().decode("utf-8", errors="replace")
                if prefix and name.endswith((".xml", ".json")):
                    (stage / (name + ".truncated.txt")).write_text(prefix + redact(text, home, work))
                    continue
                if name.endswith(".xml") and not prefix:
                    try:
                        document = ET.fromstring(text)
                    except ET.ParseError:
                        (stage / (name + ".incomplete.txt")).write_text(redact(text, home, work))
                        continue
                    for element in document.iter():
                        for key, value in list(element.attrib.items()):
                            element.set(key, redact(value, home, work).strip())
                        if element.text:
                            element.text = redact(element.text, home, work)
                        if element.tail:
                            element.tail = redact(element.tail, home, work)
                    text = ET.tostring(document, encoding="unicode")
                elif name.endswith(".json") and not prefix:
                    def clean(value):
                        if isinstance(value, str):
                            return redact(value, home, work).strip()
                        if isinstance(value, dict):
                            return {key: clean(item) for key, item in value.items()}
                        if isinstance(value, list):
                            return [clean(item) for item in value]
                        return value
                    try:
                        text = json.dumps(clean(json.loads(text)), indent=2)
                    except json.JSONDecodeError:
                        (stage / (name + ".incomplete.txt")).write_text(redact(text, home, work))
                        continue
                else:
                    text = prefix + redact(text, home, work)
                (stage / name).write_text(text)
        with output.open("xb") as stream, tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for path in sorted(stage.iterdir()):
                archive.add(path, arcname="trophy-results/" + path.name, recursive=False)


def wizard():
    print("=== Trophy Diagnostics Wizard ===")
    print("This collects system info, your Trophy log, and optionally runs tests.")
    print("Nothing is uploaded automatically.\n")
    appimage = None
    build = None
    print("Note: the public/production AppImage does not include test binaries.")
    print("      Embedded tests only work with a diagnostic build AppImage.")
    ans = input("Path to a diagnostic Trophy AppImage (optional, Enter to skip): ").strip()
    if ans:
        p = Path(ans).expanduser()
        if p.is_file():
            appimage = p
        else:
            print("Not found; skipping AppImage tests.")
    ans = input("Path to a Trophy build directory for ctest (optional, Enter to skip): ").strip()
    if ans:
        p = Path(ans).expanduser()
        if p.is_dir():
            build = p
        else:
            print("Not found; skipping ctest.")
    ans = input("Output archive path (Enter for default): ").strip()
    output = Path(ans).expanduser() if ans else None
    return appimage, build, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appimage", type=Path, help="Optional Trophy AppImage with --trophy-test support")
    parser.add_argument("--build", type=Path, help="Optional build directory for ctest")
    parser.add_argument("--output", type=Path, help="New results .tar.gz path")
    parser.add_argument("--mode", choices=("all", "tests", "info"), default="all")
    parser.add_argument("--noninteractive", action="store_true", help="Disable wizard")
    parser.add_argument("--wizard", action="store_true", help="Force interactive wizard")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine() not in ("x86_64", "amd64") or os.getuid() == 0:
        parser.error("Run as an ordinary user on x86_64 Linux, never with sudo")

    # Interactive wizard when no inputs given and stdin is a tty
    if args.wizard or (not args.noninteractive and not args.appimage and not args.build and not args.output and sys.stdin.isatty()):
        appimage, build, output = wizard()
        args.appimage = appimage
        args.build = build
        if output:
            args.output = output

    base = Path("/tmp/trophy-diag")
    base.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="trophy-diagnostics-", dir=str(base)))
    results = work / "results"
    results.mkdir(mode=0o700)
    output = (args.output or Path.cwd() / ("Trophy-diagnostics-" + time.strftime("%Y%m%d-%H%M%S") + ".tar.gz")).absolute()
    if os.path.lexists(output) or not output.parent.is_dir():
        parser.error("Report path must be new and its parent directory must exist")

    print("Local workspace (retained even on interruption):", work)
    info = system_info()
    (results / "system-info.json").write_text(json.dumps(info, indent=2) + "\n")

    log_path = find_trophy_log()
    if log_path:
        print("Found Trophy log:", log_path)
        shutil.copyfile(log_path, results / "trophy.log")
    else:
        print("No Trophy log found.")

    summary = {
        "version": "diagnostics",
        "kernel": platform.release(),
        "architecture": platform.machine(),
        "desktop": os.environ.get("XDG_CURRENT_DESKTOP", "unknown"),
        "trophy_log": str(log_path) if log_path else None,
        "tests": {"status": "not_requested"},
    }

    try:
        if args.mode in ("all", "tests") and (args.appimage or args.build):
            try:
                summary["tests"] = run_tests(args.appimage, args.build, work, results)
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, tarfile.TarError, ET.ParseError) as error:
                summary["tests"] = {"status": "blocked_or_incomplete", "reason": str(error)}
            print("tests:", summary["tests"])
        try:
            feedback = input("Feedback (optional): ")
            (results / "feedback.txt").write_text(feedback + "\n")
        except (KeyboardInterrupt, EOFError):
            (results / "feedback.txt").write_text("")
    except (KeyboardInterrupt, EOFError):
        summary["interrupted"] = True
        summary["tests"] = summary.get("tests", {"status": "interrupted"})
    finally:
        (results / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        package_report(results, output, Path.home(), work)
        print("\nReport (inspect before sending):", output)
        print("Local workspace retained:", work)

    tests_ok = summary["tests"]["status"] in ("finished", "not_requested", "failed")
    return 0 if tests_ok and not summary.get("interrupted") else 1


if __name__ == "__main__":
    sys.exit(main())
