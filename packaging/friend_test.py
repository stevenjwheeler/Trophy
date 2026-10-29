"""Run a diagnostic Trophy AppImage, then collect a private results archive."""

import argparse
import hashlib
import json
import os
from pathlib import Path
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


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def verify(bundle):
    manifest = json.loads((bundle / "bundle.json").read_text())
    if manifest.get("schema") != 2 or set(manifest.get("sha256", {})) != {
        "Trophy.AppImage", "friend_test.py", "FRIEND-TESTING.md"
    }:
        raise RuntimeError("Unsupported or incomplete bundle manifest")
    for name, expected in manifest["sha256"].items():
        path = bundle / name
        if path.is_symlink() or not path.is_file() or sha256(path) != expected:
            raise RuntimeError("Bundle checksum mismatch: " + name)
    return manifest


def check_compatibility(manifest):
    required = manifest.get("minimum_glibc")
    name, installed = platform.libc_ver()
    if required and name == "glibc" and installed:
        if tuple(map(int, installed.split("."))) < tuple(map(int, required.split("."))):
            raise RuntimeError(f"This AppImage requires glibc {required}; host has {installed}. "
                               "Ask the sender for an older-baseline build; do not upgrade system libraries for this test.")


def run_owned(command, env, log, timeout):
    """Only signal our newly-created process group; bound unattended execution."""
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


TESTS = (
    "test_EncoderTest", "test_DmaBufCudaImport", "test_VaapiVideoProcessor",
    "test_EglDisplayLease", "test_Muxer", "test_EncoderFactory", "test_AudioEncoder",
    "test_AudioDriftCorrector", "test_FramePipeline", "test_PipeWireVideoFormat",
    "test_PipeWireBufferLease", "test_PipeWireSyncWait", "test_AudioMixer",
)


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
    # These selected tests need no real bus or daemon. Do not activate services.
    env.update(DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(work / "no-session-bus"),
               DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(work / "no-system-bus"),
               PIPEWIRE_REMOTE="trophy-no-host-daemon", QT_QPA_PLATFORM="offscreen",
               APPIMAGE_EXTRACT_AND_RUN="1")
    # Offscreen Qt is not a GPU override: native EGL/CUDA/VA-API use real devices
    # and the same host driver access as the normal AppImage.
    return env


def case_status(case):
    if case.find("failure") is not None or case.find("error") is not None:
        return "failed"
    if case.find("skipped") is not None or case.get("status") == "notrun":
        return "skipped"
    return "passed"


def gpu_coverage(cases, gpus):
    statuses = {(case.get("classname", ""), case.get("name", "")): case_status(case) for case in cases}
    cuda = [("DmaBufCudaImportTest", name) for name in (
        "NvencEglImportEncodesDmaBuf", "InvalidFdFallsBackWithoutCrash",
        "RealResizedBuffersScaleOnGpuWithoutShmOrStalePixels",
        "ModifierQueryDoesNotInvalidateAnActiveImporter",
        "RetiringOneImporterDoesNotInvalidateAnother",
        "ConcurrentModifierQueriesKeepImportingWithoutFallback")]
    vaapi = [("DmaBufVaapiImportTest", "RealResizedBuffersConvertScaleAndPadOnGpuWhenSupported")]
    resize = "ResizePaths/EncoderResizeTest"
    cuda += [("MuxerTest", "NvencFullChromaWhenAvailable")]
    # Require the expected fixed number as well: missing binaries/cases cannot
    # accidentally count as complete GPU coverage.
    result = {}
    for name, keys, expected_count, parameter in (("nvidia", cuda, 9, "1"), ("vaapi", vaapi, 3, "2")):
        keys += [(resize, test + "/" + parameter) for test in (
            "DecodedFramesFitFixedCanvasWithoutStalePixels", "UnusableResizedDmaBufRequestsFallbackWithoutPacket")]
        values = [statuses.get(key, "missing") for key in keys]
        result[name] = {"status": "passed" if len(values) == expected_count and all(v == "passed" for v in values)
                        else "failed" if "failed" in values else "incomplete",
                        "passed": values.count("passed"), "required": expected_count,
                        "not_exercised": [".".join(key) for key in keys if statuses.get(key) != "passed"]}
    vendors = {gpu.get("vendor", "").lower() for gpu in gpus}
    required = (["nvidia"] if "0x10de" in vendors else []) + (["vaapi"] if vendors & {"0x1002", "0x8086"} else [])
    result["expected_for_detected_gpu"] = required
    result["complete"] = bool(required) and all(result[key]["status"] == "passed" for key in required)
    return result


def automated(bundle, work, results):
    env = test_environment(work)
    image = str(bundle / "Trophy.AppImage")
    inventory = subprocess.run([image, "--trophy-test-inventory"], env=env,
                               capture_output=True, text=True, timeout=60, check=True)
    metadata = json.loads(inventory.stdout)
    if metadata.get("schema") != 2 or metadata.get("tests") != list(TESTS):
        raise RuntimeError("Diagnostic AppImage has an unsupported test inventory")
    combined = ET.Element("testsuites")
    runs = []
    try:
        for name in TESTS:
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
    finally:
        ET.ElementTree(combined).write(results / "tests.xml", encoding="unicode")
        (results / "test-runs.json").write_text(json.dumps(runs, indent=2))
    cases = list(combined.iter("testcase"))
    failed = sum(case_status(case) == "failed" for case in cases)
    skipped = sum(case_status(case) == "skipped" for case in cases)
    coverage = gpu_coverage(cases, gpu_inventory())
    status = "failed" if failed or any(run.get("exit", 0) != 0 for run in runs) else (
        "incomplete" if any(run["status"] == "incomplete" for run in runs) else (
            "passed" if coverage["complete"] else "gpu_coverage_incomplete"))
    return {"status": status, "total": len(cases), "failed": failed, "skipped": skipped,
            "passed": len(cases) - failed - skipped, "gpu": coverage,
            "scope": metadata["scope"]}


def interactive(bundle, work, results):
    if not os.environ.get("WAYLAND_DISPLAY") or not os.environ.get("XDG_RUNTIME_DIR"):
        raise RuntimeError("Interactive preview requires a real Wayland session")
    # Avoid a second instance binding/connecting to the frozen detection socket.
    if os.path.lexists("/tmp/trophy-detection.sock") or (Path(os.environ["XDG_RUNTIME_DIR"]) / "trophy.lock").exists():
        raise RuntimeError("A Trophy socket/lock already exists. Close Trophy before testing; do not delete its socket")
    print("\nPreview uses your desktop PipeWire/portal. Recordings stay in:", work / "recordings")
    print("Install/Repair helper and Vulkan enrollment can add files to your REAL user data directory.")
    print("Leave OpenGL detection, autostart and Discord presence OFF. Never test reporters with protected/anti-cheat games.")
    print("GNOME helper discovery may require a logout/login; a single session may not suffice.")
    print("Cleanup guidance is in FRIEND-TESTING.md. Quit Trophy through its tray menu when finished.")
    if input("Launch interactive preview? Type yes: ").strip().lower() != "yes":
        return {"status": "declined"}
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("QT_", "QML_", "TROPHY_")) or key in {
            "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "APPDIR", "APPIMAGE", "ARGV0", "OWD"
        }:
            env.pop(key, None)
    for key, name in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
        directory = work / name
        directory.mkdir(mode=0o700)
        env[key] = str(directory)
    # Keep real HOME/DATA/RUNTIME and bus for Steam identity, compositor helpers,
    # external game reporter discovery and the actual portal/PipeWire session.
    settings = work / "config/trophy"
    settings.mkdir()
    (settings / "settings.json").write_text(json.dumps({
        "version": 1, "storage": {"recordingsPath": str(work / "recordings"), "autoDelete": False},
        "detection": {"autoDetect": True, "autoRecord": False, "vulkanLayer": False},
        "general": {"startOnLogin": False, "minimizeToTray": False, "instantReplay": False,
                    "updateCheck": False, "globalShortcuts": False}, "discord": {"enabled": False},
    }))
    env["APPIMAGE_EXTRACT_AND_RUN"] = "1"
    env["QT_QPA_PLATFORM"] = "wayland"
    try:
        code = run_owned([str(bundle / "Trophy.AppImage")], env, results / "preview-console.log", 14400)
    finally:
        log = settings / "trophy.log"
        if log.is_file() and not log.is_symlink():
            shutil.copyfile(log, results / "preview-app.log")
    return {"status": "exited", "exit": code, "scope": "Manual feedback, not an automated detection verdict"}


def redact(text, home, work):
    for value, replacement in ((str(work), "<TEST-DIRECTORY>"), (str(home), "<HOME>")):
        text = text.replace(value, replacement)
    # Omit entire potentially secret-bearing lines rather than guessing token syntax.
    return "\n".join("[REDACTED sensitive line]" if re.search(
        r"token|authorization|password|secret|cookie|bearer", line, re.I) else line
        for line in text.splitlines()) + "\n"


def gpu_inventory(directory=Path("/sys/class/drm")):
    """Public PCI IDs/driver names only: no serial numbers or arbitrary probes."""
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


def package_report(results, output, home, work):
    # Strict allowlist: never walk settings, videos, home or the extracted filesystem.
    allowed = {"summary.json", "feedback.txt", "tests.xml", "test-runs.json",
               "preview-console.log", "preview-app.log"}
    allowed.update(name + suffix for name in TESTS for suffix in (".xml", ".log"))
    base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
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
        # Exclusive creation: never overwrite a previous report or follow a symlink.
        with output.open("xb") as stream, tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for path in sorted(stage.iterdir()):
                archive.add(path, arcname="trophy-results/" + path.name, recursive=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("all", "tests", "preview"), default="all")
    parser.add_argument("--output", type=Path, help="New results .tar.gz path")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine() not in ("x86_64", "amd64") or os.getuid() == 0:
        parser.error("Run as an ordinary user on x86_64 Linux, never with sudo")
    bundle = Path(__file__).resolve().parent
    manifest = verify(bundle)
    compatibility_error = None
    try:
        check_compatibility(manifest)
    except RuntimeError as error:
        compatibility_error = str(error)
        print(compatibility_error)
        print("No AppImage processes will be launched; you can still create a diagnostic report.")
    print("Trusted bundle required: checksums detect damage, not a malicious publisher.")
    print("No packages will be installed. No reports will be uploaded.")
    print("Tests include real GPU encode/decode/import/resize checks. GPU skips are not a validated GPU result.")
    if input("Continue? Type yes: ").strip().lower() != "yes":
        return 0
    base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    # Retain the workspace: manual recordings and evidence must not be silently deleted.
    work = Path(tempfile.mkdtemp(prefix="trophy-friend-", dir=base))
    results = work / "results"
    results.mkdir(mode=0o700)
    output = (args.output or Path.cwd() / ("Trophy-results-" + time.strftime("%Y%m%d-%H%M%S") + ".tar.gz")).absolute()
    print("Local workspace (retained even on interruption):", work)
    if os.path.lexists(output) or not output.parent.is_dir():
        parser.error("Report path must be new and its parent directory must exist")
    summary = {"version": manifest["version"], "source_revision": manifest["source_revision"],
               "source_dirty": manifest["source_dirty"], "build_ids": manifest["build_ids"],
               "payload_sha256": manifest["sha256"], "kernel": platform.release(),
               "minimum_glibc": manifest.get("minimum_glibc"), "host_libc": platform.libc_ver(),
               "gpus": gpu_inventory(),
               "architecture": platform.machine(), "desktop": os.environ.get("XDG_CURRENT_DESKTOP", "unknown"),
               "tests": {"status": "not_requested"}, "preview": {"status": "not_requested"}}
    active_mode = None
    try:
        for mode, function in (("tests", automated), ("preview", interactive)):
            if args.mode in ("all", mode):
                if compatibility_error:
                    summary[mode] = {"status": "blocked_or_incomplete", "reason": compatibility_error}
                    continue
                active_mode = mode
                try:
                    summary[mode] = function(bundle, work, results)
                except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, tarfile.TarError, ET.ParseError) as error:
                    summary[mode] = {"status": "blocked_or_incomplete", "reason": str(error)}
                print(mode + ":", summary[mode])
                active_mode = None
        print("\nFeedback: desktop/GPU/driver, game, detection result, recording/audio result.")
        print("Do not enter passwords, tokens or private information. One line is enough.")
        (results / "feedback.txt").write_text(input("Feedback (optional): ") + "\n")
    except (KeyboardInterrupt, EOFError):
        summary["interrupted"] = True
        if active_mode:
            summary[active_mode] = {"status": "interrupted"}
    finally:
        (results / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        package_report(results, output, Path.home(), work)
        print("\nReport (inspect before sending):", output)
        print("Local workspace retained, including any recordings:", work)
        print("Detection helpers may remain. Follow FRIEND-TESTING.md before removing the workspace.")
    preview_ok = summary["preview"]["status"] in ("not_requested", "declined") or (
        summary["preview"]["status"] == "exited" and summary["preview"].get("exit") == 0)
    return 0 if summary["tests"]["status"] in ("passed", "not_requested") and preview_ok and not summary.get("interrupted") else 1


if __name__ == "__main__":
    sys.exit(main())
