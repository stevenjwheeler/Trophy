"""Daemon-free checks of the package smoke harness, not capture behavior."""

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "trophy_smoke", Path(__file__).parents[1] / "packaging/smoke_appimage.py")
smoke_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke_module)


class AppImagePayloadTest(unittest.TestCase):
    """The bundled FFmpeg must stay a single generation with x264."""

    def setUp(self):
        root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="trophy-payload-test-", dir=root)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def payload_tree(self, avcodecs=("libavcodec.so.63",), x264=True,
                     duplicate_trophy=False, extra_style=None):
        root = self.root / "payload"
        for relative in (
            "usr/bin/qt.conf",
            "usr/lib/libTrophy.so",
            "usr/lib/trophy/libtrophy_layer.so",
            "usr/lib/trophy/libtrophy_gl_shim.so",
            "usr/plugins/platforms/libqwayland-generic.so",
            "usr/plugins/platforms/libqoffscreen.so",
            "usr/plugins/wayland-shell-integration/libxdg-shell.so",
            "usr/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so",
            "usr/plugins/multimedia/libffmpegmediaplugin.so",
            "usr/qml/QtQuick/Dialogs/FolderDialog.qml",
            "usr/share/applications/io.github.stevenjwheeler.Trophy.desktop",
            "usr/share/trophy/vulkan_layer/trophy_layer.json",
            "usr/share/trophy/kwin/trophy-monitor/contents/code/main.js",
            "usr/share/metainfo/io.github.stevenjwheeler.Trophy.metainfo.xml",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("")
        for name in avcodecs:
            (root / "usr" / "lib" / name).write_text("")
        if x264:
            (root / "usr" / "lib" / "libx264.so.165").write_text("")
        if duplicate_trophy:
            (root / "usr" / "lib" / "x86_64-linux-gnu").mkdir(parents=True)
            (root / "usr" / "lib" / "x86_64-linux-gnu" / "libTrophy.so").write_text("")
        if extra_style:
            (root / "usr" / "lib" / f"libQt6QuickControls2{extra_style}.so.6").write_text("")
        return root

    def test_single_ffmpeg_generation_is_accepted(self):
        smoke_module.check_payload(self.payload_tree())

    def test_second_ffmpeg_generation_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "exactly one FFmpeg generation"):
            smoke_module.check_payload(
                self.payload_tree(avcodecs=("libavcodec.so.60", "libavcodec.so.63")))

    def test_payload_without_x264_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "no libx264"):
            smoke_module.check_payload(self.payload_tree(x264=False))

    def test_duplicate_libtrophy_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "exactly one libTrophy.so"):
            smoke_module.check_payload(self.payload_tree(duplicate_trophy=True))

    def test_unused_qt_style_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "styles the app cannot select"):
            smoke_module.check_payload(self.payload_tree(extra_style="Imagine"))


@unittest.skipUnless(shutil.which("dbus-run-session"), "private bus launcher unavailable")
class AppImageSmokeTest(unittest.TestCase):
    def setUp(self):
        root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="trophy-smoke-test-", dir=root)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.log = self.root / "smoke.log"

    def executable(self, mode):
        app = self.root / "fake-app"
        app.write_text(
            "#!/usr/bin/env python3\n"
            "import os, signal, sys, time\nfrom pathlib import Path\n"
            "Path(os.environ['XDG_RUNTIME_DIR'], 'trophy.lock').write_text(str(os.getpid()))\n"
            "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
            "print('HOME=' + os.environ['HOME'], flush=True)\n"
            + ("sys.exit(2)\n" if mode == "early-exit" else
               "print('Application initialized successfully', flush=True)\n")
             + ("print('TypeError: broken delegate', flush=True)\n" if mode == "qml-error" else "")
             + ("print('Binding loop detected for property width', flush=True)\n" if mode == "binding-loop" else "")
             + ("print('Qt Quick Layouts: Detected recursive rearrange. Aborting after two iterations.', flush=True)\n"
                if mode == "layout-loop" else "")
            + "while True: time.sleep(0.1)\n")
        app.chmod(0o755)
        return app

    def test_ready_app_shuts_down_and_uses_private_home(self):
        smoke_module.smoke(self.executable("good"), self.log)
        self.assertIn("trophy-image-smoke-", self.log.read_text())

    def test_early_exit_fails_before_readiness(self):
        with self.assertRaisesRegex(RuntimeError, "before initialization"):
            smoke_module.smoke(self.executable("early-exit"), self.log)

    def test_surviving_with_qml_error_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, "QML/platform errors"):
            smoke_module.smoke(self.executable("qml-error"), self.log)

    def test_readiness_alone_does_not_prove_a_complete_bundle(self):
        fake_image = self.root / "Incomplete.AppImage"
        self.executable("good").rename(fake_image)
        with self.assertRaisesRegex(RuntimeError, "payload is missing"):
            smoke_module.smoke(fake_image, self.log)

    def test_binding_loop_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, "QML/platform errors"):
            smoke_module.smoke(self.executable("binding-loop"), self.log)

    def test_recursive_layout_warning_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, "QML/platform errors"):
            smoke_module.smoke(self.executable("layout-loop"), self.log)


if __name__ == "__main__":
    unittest.main()
