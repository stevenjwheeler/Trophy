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


def soname_paths(mapped, soname):
    """Paths mapped for one soname family.

    A mapped file keeps its real name, so a resolved symlink shows up as
    /usr/lib/.../libva.so.2.2000.0 rather than libva.so.2. Matching the
    prefix is what makes the check work for both bundled (unversioned copies)
    and host (versioned) libraries.
    """
    paths = set()
    for name, found in mapped.items():
        if name.startswith(soname):
            paths |= found
    return paths


def check_loader(pid, payload, expect_host_libva, expect_bundled_runtime):
    """Verify the launcher resolved libraries the way the AppRun promises.

    Qt and FFmpeg must always come from the AppImage: a host Qt with the same
    soname but an older version is the bug that produced
    "version `Qt_6.12' not found". With a host libva installed, every libva
    module the process loads must come from one host directory, never mixed
    with the bundle.

    Bundle assertions only apply to AppImage payloads; a native prefix install
    links the host's Qt/FFmpeg by design, so its caller passes False.
    """
    mapped = loaded_libraries(pid)
    prefix = str(payload) + os.sep
    if expect_bundled_runtime:
        qt = soname_paths(mapped, "libQt6Core.so.6")
        if not qt:
            raise RuntimeError("libQt6Core.so.6 was never loaded")
        for soname in ("libQt6", "libavcodec", "libavformat", "libavutil"):
            shadowed = sorted(
                path
                for name, paths in mapped.items()
                if name.startswith(soname)
                for path in paths
                if not path.startswith(prefix)
            )
            if shadowed:
                raise RuntimeError(
                    f"host {soname}* shadowed the bundled copy (ABI mismatch risk): "
                    + ", ".join(shadowed))
    if expect_host_libva:
        # libavutil links libva and libva-drm. libva-x11 is optional: FFmpeg 9
        # no longer links it, so it is only checked when the process loaded it.
        parents = set()
        for lib in ("libva.so.2", "libva-drm.so.2"):
            paths = soname_paths(mapped, lib)
            if not paths:
                raise RuntimeError(f"{lib} was never loaded")
            bundled = sorted(p for p in paths if p.startswith(prefix))
            if bundled:
                raise RuntimeError(
                    f"bundled {lib} was used instead of the host's: " + ", ".join(bundled))
            parents |= {os.path.dirname(p) for p in paths}
        optional = soname_paths(mapped, "libva-x11.so.2")
        bundled = sorted(p for p in optional if p.startswith(prefix))
        if bundled:
            raise RuntimeError(
                "bundled libva-x11.so.2 was used instead of the host's: "
                + ", ".join(bundled))
        parents |= {os.path.dirname(p) for p in optional}
        if len(parents) != 1:
            raise RuntimeError(
                "libva modules came from different directories: " + ", ".join(sorted(parents)))


def check_payload(payload):
    """Verify the AppImage payload, including that it carries one FFmpeg.

    Kept separate from the process checks so the packaging harness can be
    tested against fake trees.
    """
    for pattern in (
        "usr/bin/qt.conf", "usr/lib/libTrophy.so",
        "usr/lib/**/trophy/libtrophy_layer.so",
        "usr/lib/**/trophy/libtrophy_gl_shim.so",
        "usr/plugins/platforms/libqwayland*.so",
        "usr/plugins/platforms/libqoffscreen.so",
        "usr/plugins/wayland-shell-integration/*.so",
        "usr/plugins/wayland-graphics-integration-client/*.so",
        "usr/plugins/multimedia/*.so",
        # Unregistered AppImages fall back to Qt's own folder
        # picker, which only exists in the QML payload.
        "usr/qml/**/FolderDialog.qml",
        "usr/share/applications/io.github.stevenjwheeler.Trophy.desktop",
        "usr/share/trophy/vulkan_layer/trophy_layer.json",
        "usr/share/trophy/kwin/trophy-monitor/contents/code/main.js",
        "usr/share/metainfo/*.metainfo.xml",
    ):
        if not any(payload.glob(pattern)):
            raise RuntimeError(f"AppImage payload is missing {pattern}")
    # One FFmpeg generation only: libTrophy and Qt Multimedia share the slim
    # FFmpeg 9 built by packaging/ffmpeg. A second major here means the
    # duplicate-generation bundle came back, and Qt's SDK copy has no software
    # H.264 encoder, so recording could silently lose x264.
    avcodecs = sorted({
        path.name
        for pattern in ("usr/lib*/libavcodec.so.*",
                        "usr/lib/*/libavcodec.so.*")
        for path in payload.glob(pattern)
    })
    if avcodecs != ["libavcodec.so.63"]:
        raise RuntimeError(
            "AppImage must bundle exactly one FFmpeg generation "
            f"(libavcodec.so.63), found: {avcodecs or 'none'}")
    if not any(path for pattern in ("usr/lib*/libx264.so.*", "usr/lib/*/libx264.so.*")
               for path in payload.glob(pattern)):
        raise RuntimeError(
            "AppImage bundles no libx264: software H.264 recording would regress")
    # Packaging prunes payload the app can never load (packaging/prune_appdir.py):
    # one libTrophy.so, and only the Qt Quick Controls styles the pinned Fusion
    # style needs.
    trophies = [p for p in payload.glob("usr/lib/**/libTrophy.so")
                if p.is_file() and not p.is_symlink()]
    if len(trophies) != 1:
        raise RuntimeError(
            "AppImage must contain exactly one libTrophy.so, found: "
            + (", ".join(str(p.relative_to(payload)) for p in trophies) or "none"))
    unused_styles = ("Imagine", "Material", "Universal", "FluentWinUI3")
    leftovers = [p.name for p in payload.glob("usr/lib/libQt6QuickControls2*.so.6*")
                 if any(style in p.name for style in unused_styles)]
    leftovers += [style for style in unused_styles
                  if (payload / "usr/qml/QtQuick/Controls" / style).exists()]
    if leftovers:
        raise RuntimeError(
            "AppImage bundles Qt Quick Controls styles the app cannot select: "
            + ", ".join(leftovers))


def smoke(image, log_path, expect_host_libva=False, expect_bundled_runtime=False):
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
                check_loader(app_pid, payload, expect_host_libva,
                             expect_bundled_runtime)
                if image.suffix == ".AppImage":
                    check_payload(payload)
                os.kill(app_pid, signal.SIGTERM)
                process.wait(timeout=10)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        output = log_path.read_text()
        if expect_bundled_runtime:
            # Support depends on knowing which half of the runtime was used, so
            # a bundle that stops reporting it is a regression too.
            for marker in ("Runtime libraries:", "from the host", "from the bundle"):
                if marker not in output:
                    raise RuntimeError(
                        f"startup log is missing the runtime library inventory ({marker})")
            # The bundled FFmpeg must keep the software H.264 encoder that the
            # Qt SDK's LGPL copy lacks; without x264 there is no recording
            # fallback when no GPU encoder is usable. The encoder catalogue is
            # DEBUG level, so it lands in trophy.log, not the console.
            app_log = config / "trophy.log"
            app_output = app_log.read_text(errors="replace") if app_log.exists() else ""
            if "x264" not in output and "x264" not in app_output:
                raise RuntimeError(
                    "startup log does not report the software x264 encoder; "
                    "the bundled FFmpeg lost its H.264 encoder")
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
        smoke(args.image.absolute(), args.log.resolve(), args.expect_host_libva,
              expect_bundled_runtime=True)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Smoke test failed: {error}; log: {args.log}\n")
