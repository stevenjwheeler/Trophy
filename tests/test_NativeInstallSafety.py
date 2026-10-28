"""Safety gates only: these tests must never run a real package manager."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / "packaging"))
spec = importlib.util.spec_from_file_location(
    "native_install", Path(__file__).parents[1] / "packaging/test_native_install.py")
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)


class NativeInstallSafetyTest(unittest.TestCase):
    def test_explicit_disposable_consent_required(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(install.Path, "is_file", return_value=True):
            with self.assertRaisesRegex(RuntimeError, "explicitly disposable"):
                install.require_container()

    def test_marker_alone_does_not_allow_host_install(self):
        with patch.dict(os.environ, {"TROPHY_PACKAGE_TEST_CONTAINER": "1"}), \
                patch.object(install.Path, "is_file", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "outside"):
                install.require_container()

    def test_never_escalates_to_host_sudo(self):
        with patch.dict(os.environ, {"TROPHY_PACKAGE_TEST_CONTAINER": "1"}), \
                patch.object(install.Path, "is_file", return_value=True), \
                patch.object(install.os, "geteuid", return_value=1000):
            with self.assertRaisesRegex(RuntimeError, "container root"):
                install.require_container()

    def test_guard_precedes_all_package_commands(self):
        with patch.object(install, "require_container", side_effect=RuntimeError("blocked")), \
                patch.object(install.subprocess, "run") as run, \
                patch.object(install.subprocess, "check_output") as output:
            with self.assertRaisesRegex(RuntimeError, "blocked"):
                install.test_package(Path("not-real.deb"), None, Path("unused"))
            run.assert_not_called()
            output.assert_not_called()

    def test_only_trophy_packages_are_accepted(self):
        root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        with tempfile.TemporaryDirectory(dir=root) as directory, \
                patch.object(install, "require_container"), \
                patch.object(install.subprocess, "check_output", return_value="unrelated\n"), \
                patch.object(install.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "non-Trophy"):
                install.test_package(Path("not-real.deb"), None, Path(directory))
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
