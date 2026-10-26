"""Fixture-only safety and result-classification checks for runtime tooling."""
import gzip
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

PACKAGING = Path(__file__).resolve().parents[1] / "packaging"
sys.path.insert(0, str(PACKAGING))
import check_reporter_runtimes as matrix
import materialize_reporter_runtime as materializer


class ReporterRuntimeTest(unittest.TestCase):
    def test_missing_sysroot_is_inconclusive_without_host_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(matrix.abi, "parse_ldconfig", side_effect=AssertionError("host fallback")):
                result = matrix.evaluate("empty", Path(directory), [Path("reporter.so")], {}, True)
            self.assertEqual(result["status"], "inconclusive")
            self.assertEqual(result["reporters"], [])

    def test_environment_does_not_inherit_loader_or_session_overrides(self):
        with patch.dict(matrix.os.environ, {"LD_PRELOAD": "foreign", "LD_LIBRARY_PATH": "/foreign",
                                           "DBUS_SESSION_BUS_ADDRESS": "session", "STEAM_RUNTIME": "1"}):
            env = matrix.clean_environment(Path("/scratch"))
        for name in ("LD_PRELOAD", "LD_LIBRARY_PATH", "DBUS_SESSION_BUS_ADDRESS", "STEAM_RUNTIME"):
            self.assertNotIn(name, env)
        self.assertEqual(env["DISABLE_TROPHY_DETECTION"], "1")

    def test_incompatible_reporter_is_not_executed(self):
        with patch.object(matrix.abi, "sysroot_providers", return_value={}), \
             patch.object(matrix.abi, "check", return_value={"class": "ELF32", "errors": ["missing version"]}), \
             patch.object(matrix.subprocess, "run", side_effect=AssertionError("unsafe execution")):
            result = matrix.evaluate("runtime", Path("/runtime"), [Path("shim.so")], {32: Path("probe")}, True)
        self.assertEqual(result["reporters"][0]["fixture"]["status"], "not-run-abi-unsatisfied")

    def test_static_mode_never_executes_reporter(self):
        with patch.object(matrix.abi, "sysroot_providers", return_value={}), \
             patch.object(matrix.abi, "check", return_value={"class": "ELF64", "errors": []}), \
             patch.object(matrix.subprocess, "run", side_effect=AssertionError("unsafe execution")):
            result = matrix.evaluate("runtime", Path("/runtime"), [Path("shim.so")], {}, False)
        self.assertEqual(result["reporters"][0]["fixture"]["status"], "not-requested")

    def fixture(self, root, extra=""):
        content = b"\x7fELFfixture provider"
        digest = hashlib.sha256(content).hexdigest()
        runtime = root / "runtime"
        (runtime / "files/00").mkdir(parents=True)
        (runtime / "files/00/opaque.bin").write_bytes(content)
        with gzip.open(runtime / "usr-mtree.txt.gz", "wt") as stream:
            stream.write("#mtree\n./lib/x86_64-linux-gnu/libc.so.6 type=link link=libc-real.so\n"
                         f"./lib/x86_64-linux-gnu/libc-real.so type=file size={len(content)} sha256={digest}\n" + extra)
        return runtime, content

    def test_materialization_verifies_opaque_content_and_preserves_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime, content = self.fixture(root)
            result = materializer.materialize(runtime, root / "scratch")
            self.assertEqual(len(result["copied"]), 1)
            self.assertFalse(result["missing"])
            copied = root / "scratch/usr/lib/x86_64-linux-gnu/libc.so.6"
            self.assertEqual(copied.read_bytes(), content)
            self.assertFalse(copied.is_symlink())
            self.assertEqual((runtime / "files/00/opaque.bin").read_bytes(), content)

    def test_foreign_destination_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime, _ = self.fixture(root)
            target = root / "existing"
            target.mkdir()
            with self.assertRaisesRegex(ValueError, "new scratch"):
                materializer.materialize(runtime, target)

    def test_corrupt_store_is_missing_not_replaced_with_host(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime, content = self.fixture(root)
            (runtime / "files/00/opaque.bin").write_bytes(b"x" * len(content))
            result = materializer.materialize(runtime, root / "scratch")
            self.assertFalse(result["copied"])
            self.assertEqual(result["missing"], ["lib/x86_64-linux-gnu/libc.so.6"])

    def test_unsafe_manifest_paths_and_links_are_rejected(self):
        for extra in ("../outside type=file\n", "./lib/libm.so.6 type=link link=../../outside\n",
                      "./lib/libm.so.6 type=link link=libm.so.6\n"):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                runtime, _ = self.fixture(root, extra)
                with self.assertRaises(ValueError):
                    materializer.materialize(runtime, root / "scratch")


if __name__ == "__main__":
    unittest.main()
