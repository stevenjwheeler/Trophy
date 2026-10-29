"""Diagnostics collection safety fixtures; no desktop services changed."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("collect_diagnostics", ROOT / "packaging/collect_diagnostics.py")
diag = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(diag)


class DiagnosticsTest(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").exists() else None
        self.temp = tempfile.TemporaryDirectory(prefix="trophy-diagnostics-fixture-", dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_test_environment_isolates_settings_and_services_not_gpu_devices(self):
        with mock.patch.dict(os.environ, {"LD_PRELOAD": "foreign", "LIBGL_ALWAYS_SOFTWARE": "1",
                                          "PIPEWIRE_REMOTE": "host-daemon", "GTEST_FILTER": "NoTests"}):
            env = diag.test_environment(self.root)
        for key in ("LD_PRELOAD", "LIBGL_ALWAYS_SOFTWARE", "GTEST_FILTER"):
            self.assertNotIn(key, env)
        for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR", "TMPDIR"):
            self.assertTrue(Path(env[key]).is_relative_to(self.root))
        self.assertIn(str(self.root), env["DBUS_SESSION_BUS_ADDRESS"])
        self.assertNotEqual(env["PIPEWIRE_REMOTE"], "host-daemon")
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")

    def test_gpu_inventory_excludes_connectors_and_serials(self):
        device = self.root / "card0/device"
        device.mkdir(parents=True)
        (device / "vendor").write_text("0x1002\n")
        (device / "device").write_text("0x98e4\n")
        (device / "serial").write_text("DO NOT COLLECT")
        (self.root / "card0-HDMI-A-1").mkdir()
        self.assertEqual(diag.gpu_inventory(self.root), [
            {"card": "card0", "vendor": "0x1002", "device": "0x98e4"}])

    def test_report_allowlist_redaction_and_valid_structured_results(self):
        results = self.root / "results"
        results.mkdir()
        (results / "settings.json").write_text("PRIVATE TOKEN")
        (results / "recording.mkv").write_bytes(b"video")
        (results / "test_Muxer.log").write_text("/home/friend/game\nrestore_token=private\n" + str(self.root))
        (results / "feedback.txt").write_text("feedback")
        (results / "summary.json").write_text(json.dumps({"reason": "token=private", "home": "/home/friend"}))
        (results / "tests.xml").write_text('<testsuite><testcase name="TokenTest"><failure>token=private</failure></testcase></testsuite>')
        output = self.root / "report.tar.gz"
        diag.package_report(results, output, Path("/home/friend"), self.root)
        with tarfile.open(output) as archive:
            names = set(archive.getnames())
            self.assertIn("trophy-results/test_Muxer.log", names)
            self.assertIn("trophy-results/summary.json", names)
            self.assertIn("trophy-results/tests.xml", names)
            log = archive.extractfile("trophy-results/test_Muxer.log").read().decode()
            self.assertNotIn("private", log)
            self.assertNotIn("/home/friend", log)
            self.assertNotIn(str(self.root), log)
            self.assertIn("<HOME>", log)
        with self.assertRaises(FileExistsError):
            diag.package_report(results, output, Path("/home/friend"), self.root)

    def test_owned_process_timeout_is_drained(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            diag.run_owned(["/usr/bin/sleep", "30"], {"PATH": "/usr/bin"}, self.root / "sleep.log", 0.05)

    def test_find_trophy_log_returns_none_when_absent(self):
        with mock.patch.object(diag.Path, "home", return_value=self.root):
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "xdg")}, clear=False):
                self.assertIsNone(diag.find_trophy_log())

    def test_system_info_includes_gpus(self):
        device = self.root / "card0/device"
        device.mkdir(parents=True)
        (device / "vendor").write_text("0x10de\n")
        (device / "device").write_text("0x2704\n")
        with mock.patch.object(diag, "gpu_inventory", return_value=diag.gpu_inventory(self.root)):
            info = diag.system_info()
        self.assertIn("gpus", info)
        self.assertIn("kernel", info)
        self.assertIn("architecture", info)


if __name__ == "__main__":
    unittest.main()
