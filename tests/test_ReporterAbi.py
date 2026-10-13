"""Fixture-only coverage of the static reporter ABI checker."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "reporter_abi", Path(__file__).resolve().parents[1] / "packaging/check_reporter_abi.py")
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)

HEADER = "Class: ELF64\nMachine: Advanced Micro Devices X86-64\n"
CONSUMER = HEADER + """
 0x1 (NEEDED) Shared library: [libfixture.so.1]
Symbol table '.dynsym' contains 2 entries:
 1: 0000000000000000 0 FUNC GLOBAL DEFAULT UND fixture@FIXTURE_1.0 (2)
 2: 0000000000000000 0 FUNC WEAK DEFAULT UND optional@FIXTURE_1.0 (2)
Version needs section '.gnu.version_r' contains 1 entry:
 000000: Version: 1 File: libfixture.so.1 Cnt: 1
 0x0010: Name: FIXTURE_1.0 Flags: none Version: 2
"""
PROVIDER = HEADER + """
Symbol table '.dynsym' contains 1 entry:
 1: 0000000000001234 8 FUNC GLOBAL DEFAULT 12 fixture@@FIXTURE_1.0
Version definition section '.gnu.version_d' contains 1 entry:
 0x001c: Rev: 1 Flags: none Index: 2 Cnt: 1 Name: FIXTURE_1.0
"""


class ReporterAbiTest(unittest.TestCase):
    def test_parse_requirements_and_weak_symbols(self):
        obj = abi.parse_elf(CONSUMER)
        self.assertEqual(obj["needed"], ["libfixture.so.1"])
        self.assertEqual(obj["requirements"], {"libfixture.so.1": {"FIXTURE_1.0"}})
        self.assertEqual(obj["undefined"], [("fixture", "FIXTURE_1.0", "libfixture.so.1")])

    def test_default_export_normalized(self):
        obj = abi.parse_elf(PROVIDER)
        self.assertIn("fixture@FIXTURE_1.0", obj["exports"])
        self.assertEqual(obj["definitions"], {"FIXTURE_1.0"})

    def test_same_version_name_uses_index_not_name(self):
        text = CONSUMER + """
 000020: Version: 1 File: libother.so.1 Cnt: 1
 0x0030: Name: FIXTURE_1.0 Flags: none Version: 3
"""
        self.assertEqual(abi.parse_elf(text)["undefined"][0][2], "libfixture.so.1")

    def test_bad_headers_and_incomplete_versions_rejected(self):
        variants = ("garbage", HEADER.replace("ELF64", "ELF128"),
                    CONSUMER.replace("File: libfixture.so.1 Cnt: 1", "garbage"),
                    CONSUMER.replace("Flags: none Version: 2", "Flags: none"),
                    CONSUMER.replace(" (2)", ""),
                    CONSUMER.replace(" (2)", " (99)"),
                    CONSUMER.replace("[libfixture.so.1]", "[/foreign/libfixture.so.1]"))
        for text in variants:
            with self.subTest(text=text), self.assertRaises(abi.CheckError):
                abi.parse_elf(text)

    def test_ldconfig_paths_and_duplicates(self):
        result = abi.parse_ldconfig("libfixture.so.1 (libc6,x86-64) => /lib/libfixture.so.1\n"
                                   "libfixture.so.1 (libc6) => /lib32/libfixture.so.1\n")
        self.assertEqual(len(result["libfixture.so.1"]), 2)
        with self.assertRaises(abi.CheckError):
            abi.parse_ldconfig("invalid output")

    def check_fixture(self, provider=PROVIDER, candidates=None):
        objects = {Path("/fixture/reporter.so"): abi.parse_elf(CONSUMER),
                   Path("/fixture/libfixture.so.1"): abi.parse_elf(provider)}
        with patch.object(abi, "inspect", side_effect=lambda path: objects[Path(path)]):
            return abi.check(Path("/fixture/reporter.so"), {
                "libfixture.so.1": candidates if candidates is not None else [Path("/fixture/libfixture.so.1")]})

    def test_complete_fixture_passes(self):
        self.assertEqual(self.check_fixture()["status"], "static-check-pass")

    def test_missing_version_and_symbol_fail(self):
        report = self.check_fixture(PROVIDER.replace("FIXTURE_1.0", "FIXTURE_0.9"))
        self.assertTrue(any("missing versions" in error for error in report["errors"]))
        self.assertTrue(any("missing symbol" in error for error in report["errors"]))

    def test_architecture_mismatch_and_missing_provider(self):
        for report in (self.check_fixture(PROVIDER.replace("ELF64", "ELF32")),
                       self.check_fixture(PROVIDER.replace("Advanced Micro Devices X86-64", "AArch64")),
                       self.check_fixture(candidates=[])):
            self.assertEqual(report["status"], "static-check-fail")

    def test_duplicate_same_path_is_not_ambiguous(self):
        path = Path("/fixture/libfixture.so.1")
        self.assertEqual(self.check_fixture(candidates=[path, path])["status"], "static-check-pass")

    def test_inspection_bound(self):
        with patch.object(abi, "MAX_OBJECTS", 1), self.assertRaises(abi.CheckError):
            self.check_fixture()

    def test_ambiguous_distinct_providers_rejected(self):
        reporter = Path("/fixture/reporter.so")
        paths = [Path("/fixture/one.so"), Path("/fixture/two.so")]
        with patch.object(abi, "inspect", side_effect=lambda path: abi.parse_elf(
                CONSUMER if path == reporter else PROVIDER)):
            report = abi.check(reporter, {"libfixture.so.1": paths})
        self.assertIn("found 2", report["errors"][0])

    def test_transitive_dependency_checked(self):
        report = self.check_fixture(PROVIDER + "\n 0x1 (NEEDED) Shared library: [libmissing.so.1]\n")
        self.assertEqual(report["status"], "static-check-fail")
        self.assertIn("libmissing.so.1", report["errors"][0])

    def test_hidden_exports_do_not_satisfy_requirements(self):
        self.assertEqual(self.check_fixture(PROVIDER.replace("DEFAULT", "HIDDEN"))["status"],
                         "static-check-fail")

    def test_sysroot_search_bound(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            (root / "lib").mkdir()
            with patch.object(abi.os, "walk", return_value=[(str(root / "lib"), [], ["x"] * 20001)]):
                with self.assertRaises(abi.CheckError):
                    abi.sysroot_providers(root)

    def test_output_bound(self):
        def emit(argv, **kwargs):
            kwargs["stdout"].write(b"oversized")
            return subprocess.CompletedProcess(argv, 0)
        with patch.object(abi, "MAX_OUTPUT", 2), patch.object(abi.subprocess, "run", side_effect=emit):
            with self.assertRaises(abi.CheckError):
                abi.run_tool(["readelf"])

    def test_empty_sysroot_no_host_fallback(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            with self.assertRaises(abi.CheckError):
                abi.sysroot_providers(Path(directory))

    def test_absolute_sysroot_symlink_maps_inside_root(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            (root / "usr/lib").mkdir(parents=True)
            (root / "usr/lib/libfixture.so.1.0").write_bytes(b"fixture")
            (root / "usr/lib/libfixture.so.1").symlink_to("/usr/lib/libfixture.so.1.0")
            self.assertEqual(abi.sysroot_providers(root)["libfixture.so.1"],
                             [root / "usr/lib/libfixture.so.1.0"])

    def test_sysroot_escape_and_symlink_loop(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            (root / "lib").mkdir()
            link = root / "lib/libfixture.so.1"
            link.symlink_to("../../escape.so")
            with self.assertRaises(abi.CheckError):
                abi.sysroot_providers(root)
            link.unlink()
            link.symlink_to("libfixture.so.1")
            with self.assertRaises(abi.CheckError):
                abi.sysroot_providers(root)

    def test_tool_failures_are_explicit(self):
        for error in (OSError("missing tool"), subprocess.TimeoutExpired("readelf", 15)):
            with patch.object(abi.subprocess, "run", side_effect=error), self.assertRaises(abi.CheckError):
                abi.run_tool(["readelf"])
        with patch.object(abi.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)), self.assertRaises(abi.CheckError):
            abi.run_tool(["readelf"])


if __name__ == "__main__":
    unittest.main()
