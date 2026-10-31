"""Isolated headless AppImage startup/shutdown check (no capture consent)."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time


def loaded_libraries(pid):
    """Map library basename -> paths mapped into the running process."""
    mapped = {}
    for line in Path(f"/proc/{pid}/maps").read_text().splitlines():
        fields = line.split(None, 5)
        if len(fields) < 6:
            continue
        path = fields[5].strip()
        if path.endswith(" (deleted)"):
            path = path[: -len(" (deleted)")]
        if path.startswith("/"):
            mapped.setdefault(os.path.basename(path), set()).add(path)
    return mapped


def check_loader(pid, payload, expect_host_libva):
    """Verify the launcher resolved libraries the way the AppRun promises.

    Qt must always come from the AppImage (a host Qt with the same soname but
    an older version is the bug that produced "version `Qt_6.12' not found").
    With a host libva installed, libva must come from the host, not the bundle.
    """
    mapped = loaded_libraries(pid)
    prefix = str(payload) + os.sep
    qt = mapped.get("libQt6Core.so.6", set())
    if not qt:
        raise RuntimeError("libQt6Core.so.6 was never loaded")
    host_qt = sorted(p for p in qt if not p.startswith(prefix))
    if host_qt:
        raise RuntimeError(
            "host Qt shadowed the bundled Qt (ABI mismatch risk): " + ", ".join(host_qt))
    if expect_host_libva:
        if not mapped.get("libva.so.2"):
            raise RuntimeError("libva.so.2 was never loaded")
        for lib in ("libva.so.2", "libva-drm.so.2", "libva-x11.so.2"):
            bundled = sorted(p for p in mapped.get(lib, set()) if p.startswith(prefix))
            if bundled:
                raise RuntimeError(
                    f"bundled {lib} was used instead of the host's: " + ", ".join(bundled))


def smoke(image, log_path, expect_host_libva=False):
    if not shutil.which("dbus-run-session"):
        raise RuntimeError("dbus-run-session is required; refusing the live session bus")
    temp_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="trophy-image-smoke-", dir=temp_root) as directory:
        root = Path(directory)
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(("QT_", "QML_")) or key in {
                "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "DISPLAY", "WAYLAND_DISPLAY",
                "DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID",
                "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION",
                "KDE_FULL_SESSION", "GNOME_DESKTOP_SESSION_ID",
                "APPDIR", "APPIMAGE", "ARGV0", "OWD",
            }:
                env.pop(key, None)
        for key, subdir in {
            "HOME": "home", "XDG_CONFIG_HOME": "config", "XDG_DATA_HOME": "data",
            "XDG_CACHE_HOME": "cache", "XDG_RUNTIME_DIR": "runtime", "TMPDIR": "tmp",
        }.items():
            path = root / subdir
            path.mkdir(mode=0o700)
            env[key] = str(path)
        env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   APPIMAGE_EXTRACT_AND_RUN="1")
        # No standard service directories: do not activate host portal/KWallet
        # daemons just to verify a headless package.
        bus_config = root / "bus.conf"
        bus_config.write_text(
            '<busconfig><type>session</type>'
            f'<listen>unix:tmpdir={env["XDG_RUNTIME_DIR"]}</listen>'
            '<auth>EXTERNAL</auth><policy context="default">'
            '<allow own="*"/><allow send_destination="*"/>'
            '<allow receive_sender="*"/></policy></busconfig>')
        # Keep all storage/settings and optional integrations away from the host.
        config = Path(env["XDG_CONFIG_HOME"]) / "trophy"
        config.mkdir()
        (config / "settings.json").write_text(json.dumps({
            "version": 1,
            "storage": {"recordingsPath": str(root / "recordings"), "autoDelete": False},
            "detection": {"autoDetect": False, "autoRecord": False,
                          "windowTracking": False, "vulkanLayer": False},
            "general": {"updateCheck": False, "globalShortcuts": False,
                        "instantReplay": False, "startOnLogin": False},
        }))
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w") as log:
            process = subprocess.Popen(["dbus-run-session", "--config-file", str(bus_config),
                                        "--", str(image)],
                                       env=env, stdout=log, stderr=log,
                                       start_new_session=True)
            try:
                deadline = time.monotonic() + 30
                while "Application initialized successfully" not in log_path.read_text():
                    if process.poll() is not None:
                        raise RuntimeError(f"AppImage exited before initialization ({process.returncode})")
                    if time.monotonic() >= deadline:
                        raise RuntimeError("AppImage initialization timed out")
                    time.sleep(0.1)
                # Let queued QML errors and early startup failures surface.
                time.sleep(1)
                if process.poll() is not None:
                    raise RuntimeError("AppImage exited immediately after initialization")
                # Terminate the actual app, leaving the runtime alive to reap
                # it and finish extract-and-run cleanup before XDG removal.
                lock = Path(env["XDG_RUNTIME_DIR"]) / "trophy.lock"
                app_pid = int(lock.read_text().splitlines()[0])
                if app_pid <= 1 or os.getpgid(app_pid) != process.pid:
                    raise RuntimeError("App lock does not belong to the smoke process group")
                payload = Path(os.readlink(f"/proc/{app_pid}/exe")).parents[2]
                check_loader(app_pid, payload, expect_host_libva)
                if image.suffix == ".AppImage":
                    for pattern in (
                        "usr/bin/qt.conf", "usr/lib/libTrophy.so",
                        "usr/lib/**/trophy/libtrophy_layer.so",
                        "usr/lib/**/trophy/libtrophy_gl_shim.so",
                        "usr/plugins/platforms/libqwayland*.so",
                        "usr/plugins/platforms/libqoffscreen.so",
                        "usr/plugins/wayland-shell-integration/*.so",
                        "usr/plugins/wayland-graphics-integration-client/*.so",
                        "usr/plugins/multimedia/*.so",
                        "usr/share/applications/trophy.desktop",
                        "usr/share/trophy/vulkan_layer/trophy_layer.json",
                        "usr/share/trophy/kwin/trophy-monitor/contents/code/main.js",
                        "usr/share/metainfo/*.metainfo.xml",
                    ):
                        if not any(payload.glob(pattern)):
                            raise RuntimeError(f"AppImage payload is missing {pattern}")
                os.kill(app_pid, signal.SIGTERM)
                process.wait(timeout=10)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        output = log_path.read_text()
        if re.search(r"ReferenceError|TypeError|Cannot assign to|"
                      r"QQmlApplicationEngine failed|Failed to initialize application|"
                      r"Binding loop detected|Detected recursive rearrange|"
                     r"could not (?:find|load) the Qt platform plugin", output, re.IGNORECASE):
            raise RuntimeError("AppImage startup reported QML/platform errors")
        if process.returncode not in (0, 128 + signal.SIGTERM, -signal.SIGTERM):
            raise RuntimeError(f"AppImage shutdown failed ({process.returncode})")
        print(f"AppImage initialized and shut down in isolation; log: {log_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--log", type=Path, default=Path("appimage-smoke.log"))
    parser.add_argument("--expect-host-libva", action="store_true",
                        help="require libva to resolve to the host, not the bundle")
    args = parser.parse_args()
    try:
        if not args.image.is_file():
            raise RuntimeError("AppImage does not exist")
        # Preserve the entry path: resolving an AppRun symlink here would
        # bypass the launcher and hide packaging defects.
        smoke(args.image.absolute(), args.log.resolve(), args.expect_host_libva)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Smoke test failed: {error}; log: {args.log}\n")
