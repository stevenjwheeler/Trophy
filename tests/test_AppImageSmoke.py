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


if __name__ == "__main__":
    unittest.main()
