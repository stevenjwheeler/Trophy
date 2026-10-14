#!/usr/bin/env python3
"""Run a bounded diagnostic with isolated XDG paths and a no-activation bus.

Usage: python3 run_qt_leak_probe.py [--hardware-discovery] COMMAND [ARGS...]
Hardware discovery is a comparison only; it must not be used for capture.
No sanitizer suppressions are installed and leak detection remains enabled.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    args = sys.argv[1:]
    hardware = bool(args and args[0] == "--hardware-discovery")
    if hardware:
        args.pop(0)
    if not args:
        raise SystemExit(__doc__)
    with tempfile.TemporaryDirectory(prefix="qt-leak-", dir="/tmp/opencode") as tmp:
        root = Path(tmp)
        env = os.environ.copy()
        for variable, directory in [("HOME", "home"),
                                    ("XDG_CONFIG_HOME", "config"),
                                    ("XDG_DATA_HOME", "data"),
                                    ("XDG_CACHE_HOME", "cache"),
                                    ("XDG_RUNTIME_DIR", "runtime")]:
            path = root / directory
            path.mkdir(mode=0o700)
            env[variable] = str(path)
        env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   QT_MEDIA_BACKEND="ffmpeg", QT_DISABLE_HW_TEXTURES_CONVERSION="1",
                   ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        for key in ("QT_FFMPEG_DECODING_HW_DEVICE_TYPES",
                    "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"):
            if hardware:
                env.pop(key, None)
            else:
                env[key] = ","
        # No standard service directories: the private bus cannot activate
        # portals, accessibility services, or user-session media services.
        config = root / "bus.conf"
        config.write_text(f'''<busconfig>
<type>session</type><listen>unix:tmpdir={tmp}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy>
</busconfig>''')
        env.pop("DBUS_SESSION_BUS_ADDRESS", None)
        env.pop("DBUS_SESSION_BUS_PID", None)
        env.pop("LSAN_OPTIONS", None)
        with subprocess.Popen(["dbus-daemon", "--nofork", "--print-address=1",
                               f"--config-file={config}"], stdout=subprocess.PIPE,
                              text=True, env=env) as bus:
            try:
                env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().strip()
                if not env["DBUS_SESSION_BUS_ADDRESS"]:
                    raise RuntimeError("private bus did not supply an address")
                return subprocess.run(args, env=env, timeout=60).returncode
            finally:
                bus.terminate()
                bus.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
