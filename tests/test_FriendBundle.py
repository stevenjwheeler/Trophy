"""Diagnostic kit safety and GPU coverage fixtures; no desktop services changed."""

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
SPEC = importlib.util.spec_from_file_location("friend_test", ROOT / "packaging/friend_test.py")
kit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(kit)


class FriendBundleTest(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").exists() else None
        self.temp = tempfile.TemporaryDirectory(prefix="trophy-friend-fixture-", dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_manifest_requires_all_payloads_and_rejects_damage(self):
        names = ("Trophy.AppImage", "friend_test.py", "FRIEND-TESTING.md")
        for name in names:
            (self.root / name).write_text(name)
        manifest = {"schema": 2, "sha256": {name: kit.sha256(self.root / name) for name in names}}
        (self.root / "bundle.json").write_text(json.dumps(manifest))
        self.assertEqual(kit.verify(self.root), manifest)
        (self.root / names[0]).write_text("damaged")
        with self.assertRaisesRegex(RuntimeError, "checksum"):
            kit.verify(self.root)

    def test_manifest_cannot_select_arbitrary_paths(self):
        (self.root / "bundle.json").write_text(json.dumps({"schema": 2, "sha256": {"../private": "bad"}}))
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            kit.verify(self.root)

    def test_older_glibc_is_rejected_without_system_changes(self):
        with mock.patch.object(kit.platform, "libc_ver", return_value=("glibc", "2.39")):
            with self.assertRaisesRegex(RuntimeError, "older-baseline build"):
                kit.check_compatibility({"minimum_glibc": "2.44"})
            kit.check_compatibility({"minimum_glibc": "2.39"})

    def test_incompatible_host_still_creates_report_without_launching_payload(self):
        work = self.root / "work"
        work.mkdir()
        output = self.root / "report.tar.gz"
        manifest = {"minimum_glibc": "2.44", "version": "fixture", "source_revision": "fixture",
                    "source_dirty": False, "build_ids": {}, "sha256": {}}
        with mock.patch.object(kit, "verify", return_value=manifest), \
             mock.patch.object(kit.platform, "libc_ver", return_value=("glibc", "2.39")), \
             mock.patch.object(kit.os, "getuid", return_value=1000), \
             mock.patch.object(kit.tempfile, "mkdtemp", return_value=str(work)), \
             mock.patch("sys.argv", ["friend_test.py", "--mode", "tests", "--output", str(output)]), \
             mock.patch("builtins.input", side_effect=["yes", "unsupported baseline"]), \
             mock.patch.object(kit, "automated") as automated:
            self.assertEqual(kit.main(), 1)
            automated.assert_not_called()
        with tarfile.open(output) as archive:
            summary = json.load(archive.extractfile("trophy-results/summary.json"))
            self.assertEqual(summary["tests"]["status"], "blocked_or_incomplete")

    def test_automated_runner_retains_missing_results_and_runs_remaining_binaries(self):
        results = self.root / "results"
        results.mkdir()
        inventory = subprocess.CompletedProcess([], 0, json.dumps({"schema": 2, "tests": list(kit.TESTS), "scope": "fixture"}), "")
        calls = []
        def run(command, env, log, timeout):
            name = command[2]
            calls.append(name)
            if name == kit.TESTS[0]:
                return 0  # no XML must not become a pass
            xml = Path(command[3].split("xml:", 1)[1])
            root = ET.Element("testsuites")
            suite = ET.SubElement(root, "testsuite", name="Fixture")
            suite.append(ET.Element("testcase", classname="Fixture", name=name))
            ET.ElementTree(root).write(xml)
            return 0
        with mock.patch.object(kit.subprocess, "run", return_value=inventory), \
             mock.patch.object(kit, "run_owned", side_effect=run):
            report = kit.automated(self.root, self.root, results)
        self.assertEqual(calls, list(kit.TESTS))
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["total"], len(kit.TESTS) - 1)
        self.assertEqual(json.loads((results / "test-runs.json").read_text())[0]["status"], "incomplete")

    def test_test_environment_isolates_settings_and_services_not_gpu_devices(self):
        with mock.patch.dict(os.environ, {"LD_PRELOAD": "foreign", "LIBGL_ALWAYS_SOFTWARE": "1",
                                          "PIPEWIRE_REMOTE": "host-daemon", "GTEST_FILTER": "NoTests"}):
            env = kit.test_environment(self.root)
        for key in ("LD_PRELOAD", "LIBGL_ALWAYS_SOFTWARE", "GTEST_FILTER"):
            self.assertNotIn(key, env)
        for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR", "TMPDIR"):
            self.assertTrue(Path(env[key]).is_relative_to(self.root))
        self.assertIn(str(self.root), env["DBUS_SESSION_BUS_ADDRESS"])
        self.assertNotEqual(env["PIPEWIRE_REMOTE"], "host-daemon")
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
        self.assertNotIn("QT_QUICK_BACKEND", env)

    def test_gpu_inventory_excludes_connectors_and_serials(self):
        device = self.root / "card0/device"
        device.mkdir(parents=True)
        (device / "vendor").write_text("0x1002\n")
        (device / "device").write_text("0x98e4\n")
        (device / "serial").write_text("DO NOT COLLECT")
        (self.root / "card0-HDMI-A-1").mkdir()
        self.assertEqual(kit.gpu_inventory(self.root), [
            {"card": "card0", "vendor": "0x1002", "device": "0x98e4"}])

    @staticmethod
    def nvidia_cases():
        names = ("NvencEglImportEncodesDmaBuf", "InvalidFdFallsBackWithoutCrash",
                 "RealResizedBuffersScaleOnGpuWithoutShmOrStalePixels",
                 "ModifierQueryDoesNotInvalidateAnActiveImporter",
                 "RetiringOneImporterDoesNotInvalidateAnother",
                 "ConcurrentModifierQueriesKeepImportingWithoutFallback")
        cases = [ET.Element("testcase", classname="DmaBufCudaImportTest", name=name) for name in names]
        cases.append(ET.Element("testcase", classname="MuxerTest", name="NvencFullChromaWhenAvailable"))
        cases += [ET.Element("testcase", classname="ResizePaths/EncoderResizeTest", name=name + "/1") for name in (
            "DecodedFramesFitFixedCanvasWithoutStalePixels", "UnusableResizedDmaBufRequestsFallbackWithoutPacket")]
        return cases

    def test_nvidia_requires_all_real_gpu_cases_to_pass(self):
        cases = self.nvidia_cases()
        self.assertTrue(kit.gpu_coverage(cases, [{"vendor": "0x10de"}])["complete"])
        ET.SubElement(cases[0], "skipped", message="No EGL device")
        report = kit.gpu_coverage(cases, [{"vendor": "0x10de"}])
        self.assertFalse(report["complete"])
        self.assertEqual(report["nvidia"]["status"], "incomplete")
        self.assertEqual(report["nvidia"]["passed"], 8)

    def test_missing_gpu_case_or_unknown_gpu_cannot_look_validated(self):
        self.assertFalse(kit.gpu_coverage(self.nvidia_cases()[:-1], [{"vendor": "0x10de"}])["complete"])
        self.assertFalse(kit.gpu_coverage(self.nvidia_cases(), [])["complete"])

    def test_amd_and_intel_require_vaapi_import_and_both_resize_cases(self):
        cases = [ET.Element("testcase", classname="DmaBufVaapiImportTest",
                            name="RealResizedBuffersConvertScaleAndPadOnGpuWhenSupported")]
        cases += [ET.Element("testcase", classname="ResizePaths/EncoderResizeTest", name=name + "/2") for name in (
            "DecodedFramesFitFixedCanvasWithoutStalePixels", "UnusableResizedDmaBufRequestsFallbackWithoutPacket")]
        for vendor in ("0x1002", "0x8086"):
            self.assertTrue(kit.gpu_coverage(cases, [{"vendor": vendor}])["complete"])
        ET.SubElement(cases[0], "failure")
        self.assertEqual(kit.gpu_coverage(cases, [{"vendor": "0x1002"}])["vaapi"]["status"], "failed")

    def test_diagnostic_launcher_rejects_arbitrary_binary_paths(self):
        result = subprocess.run(["sh", str(ROOT / "packaging/DiagnosticAppRun"), "--trophy-test", "../../bin/sh"],
                                env={"PATH": "/usr/bin:/bin", "APPDIR": str(self.root)}, capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_preview_refuses_existing_socket_before_launch(self):
        with mock.patch.dict(os.environ, {"WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": str(self.root)}), \
             mock.patch.object(kit.os.path, "lexists", return_value=True), \
             mock.patch.object(kit, "run_owned") as run:
            with self.assertRaisesRegex(RuntimeError, "already exists"):
                kit.interactive(self.root, self.root, self.root)
            run.assert_not_called()

    def test_report_allowlist_redaction_and_valid_structured_results(self):
        results = self.root / "results"
        results.mkdir()
        (results / "settings.json").write_text("PRIVATE TOKEN")
        (results / "recording.mkv").write_bytes(b"video")
        (results / "test_Muxer.log").write_text("/home/friend/game\nrestore_token=private\n" + str(self.root))
        (results / "feedback.txt").symlink_to(results / "settings.json")
        (results / "summary.json").write_text(json.dumps({"reason": "token=private", "home": "/home/friend"}))
        (results / "tests.xml").write_text('<testsuite><testcase name="TokenTest"><failure>token=private</failure></testcase></testsuite>')
        output = self.root / "report.tar.gz"
        kit.package_report(results, output, Path("/home/friend"), self.root)
        with tarfile.open(output) as archive:
            self.assertEqual(set(archive.getnames()), {"trophy-results/test_Muxer.log", "trophy-results/summary.json", "trophy-results/tests.xml"})
            log = archive.extractfile("trophy-results/test_Muxer.log").read().decode()
            self.assertNotIn("private", log)
            self.assertNotIn("/home/friend", log)
            self.assertNotIn(str(self.root), log)
            self.assertIn("<HOME>", log)
            self.assertNotIn("private", json.load(archive.extractfile("trophy-results/summary.json"))["reason"])
            self.assertEqual(len(list(ET.fromstring(archive.extractfile("trophy-results/tests.xml").read()).iter("testcase"))), 1)
        with self.assertRaises(FileExistsError):
            kit.package_report(results, output, Path("/home/friend"), self.root)

    def test_incomplete_junit_still_packages_diagnostics(self):
        (self.root / "tests.xml").write_text("<testsuite><testcase")
        output = self.root / "report.tar.gz"
        kit.package_report(self.root, output, Path.home(), self.root)
        with tarfile.open(output) as archive:
            self.assertEqual(archive.getnames(), ["trophy-results/tests.xml.incomplete.txt"])

    def test_owned_process_timeout_is_drained(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            kit.run_owned(["/usr/bin/sleep", "30"], {"PATH": "/usr/bin"}, self.root / "sleep.log", 0.05)


if __name__ == "__main__":
    unittest.main()
