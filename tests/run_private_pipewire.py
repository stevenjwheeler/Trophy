"""Run native audio regressions against owned, hardware-free private services."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

if not all(shutil.which(tool) for tool in ("pipewire", "wireplumber", "pw-cli", "pw-dump", "dbus-run-session")):
    print("SKIP: optional PipeWire/WirePlumber test tools unavailable")
    sys.exit(77)
version = subprocess.run(["wireplumber", "--version"], capture_output=True, text=True, timeout=5)
match = re.search(r"(?:WirePlumber|wireplumber)\s+(\d+)\.(\d+)", version.stdout)
if not match or tuple(map(int, match.groups())) < (0, 5):
    print("SKIP: isolated policy profile requires WirePlumber >= 0.5")
    sys.exit(77)
if os.environ.get("TROPHY_PRIVATE_AUDIO_BUS") != "1":
    env = dict(os.environ, TROPHY_PRIVATE_AUDIO_BUS="1")
    config = Path(__file__).with_name("private-session-bus.conf")
    sys.exit(subprocess.call(["dbus-run-session", "--config-file=" + str(config),
                             "--", sys.executable, __file__, *sys.argv[1:]], env=env))

with tempfile.TemporaryDirectory(prefix="trophy-audio-group-") as root:
    env = dict(os.environ, XDG_RUNTIME_DIR=root, XDG_CONFIG_HOME=root + "/config",
               XDG_STATE_HOME=root + "/state", XDG_DATA_HOME=root + "/data",
               HOME=root, TROPHY_PRIVATE_PIPEWIRE_TEST="1",
               DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + root + "/no-system-bus")
    # Never inherit a host remote/socket override.
    for key in ("PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_CORE",
                "PIPEWIRE_CONFIG_DIR", "PIPEWIRE_CONFIG_NAME", "WIREPLUMBER_CONFIG_DIR"):
        env.pop(key, None)
    processes = []
    logs = []
    control = Path(root) / "test-control"
    control.mkdir()
    env["TROPHY_PRIVATE_PIPEWIRE_CONTROL"] = str(control)
    result = 1
    def stop_services():
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        processes.clear()

    def start_services():
        for tool in ("pipewire", "wireplumber"):
            log = open(Path(root) / (tool + f"-{len(logs)}.log"), "w+")
            logs.append(log)
            # Policy only: never enumerate/open ALSA, Bluetooth or cameras.
            command = [tool, "--profile=policy"] if tool == "wireplumber" else [tool]
            processes.append(subprocess.Popen(command, env=env, stdout=log, stderr=log))
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if processes[-1].poll() is not None:
                    raise RuntimeError(tool + " exited before readiness")
                if tool == "pipewire":
                    ready = (Path(root) / "pipewire-0").exists()
                else:
                    probe = subprocess.run(["pw-dump"], env=env, capture_output=True, timeout=3)
                    ready = probe.returncode == 0 and any(
                        obj.get("info", {}).get("props", {}).get("application.name", "").startswith("WirePlumber")
                        for obj in json.loads(probe.stdout))
                if ready:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError(tool + " readiness timed out")
        subprocess.run(["pw-cli", "create-node", "adapter",
                        "{ factory.name=support.null-audio-sink node.name=group-test-sink "
                        "media.class=Audio/Sink audio.position=[ FL FR ] node.driver=true "
                        "node.always-process=true object.linger=true }"],
                       env=env, check=True, timeout=5)
    child = None
    try:
        start_services()
        child = subprocess.Popen(sys.argv[1:], env=env)
        deadline = time.monotonic() + 90
        while child.poll() is None:
            if time.monotonic() > deadline:
                raise RuntimeError("private audio test timed out")
            request = control / "request"
            if request.exists():
                command = request.read_text().strip()
                request.unlink()
                # Only these launcher-owned Popen objects can be stopped. No
                # PID supplied by a test, host socket or system service is used.
                if command == "stop":
                    stop_services()
                elif command == "start" and not processes:
                    start_services()
                else:
                    raise RuntimeError("invalid private-service request: " + command)
                (control / "reply").write_text(command)
            time.sleep(0.01)
        result = child.returncode
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
        stop_services()
        if result:
            for log in logs:
                log.seek(0)
                print(log.read())
        for log in logs:
            log.close()
    sys.exit(result)
