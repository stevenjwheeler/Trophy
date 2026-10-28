"""Native packaging regressions use synthetic files; never run their fake ELFs."""

import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / "packaging"))
spec = importlib.util.spec_from_file_location(
    "native_smoke", Path(__file__).parents[1] / "packaging/smoke_native.py")
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


class NativePackageSmokeTest(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.temp = tempfile.TemporaryDirectory(prefix="native fixtures ", dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "usr"

    def payload(self, libdir="lib"):
        self.libdir = libdir
        for relative in (
            "bin/trophy", f"{libdir}/libTrophy.so",
            f"{libdir}/trophy/libtrophy_layer.so", f"{libdir}/trophy/libtrophy_gl_shim.so",
            "share/applications/trophy.desktop", "share/icons/hicolor/scalable/apps/trophy.svg",
            "share/trophy/kwin/trophy-monitor/metadata.json",
            "share/trophy/kwin/trophy-monitor/contents/code/main.js",
            "share/licenses/trophy/default-app-icons/LICENSE",
            "share/licenses/trophy/default-app-icons/README.md",
        ):
            path = self.prefix / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
            path.chmod(0o755 if relative.startswith(("bin/", "lib")) else 0o644)
        path = self.prefix / "share/trophy/vulkan_layer/trophy_layer.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps({"layer": {"library_path": "/build/template.so"}}))
        path = self.prefix / "share/metainfo/io.github.stevenjwheeler.Trophy.metainfo.xml"
        path.parent.mkdir(parents=True)
        path.write_text('<component><launchable type="desktop-id">trophy.desktop</launchable></component>')

    def output(self, args):
        if args[0] == "readelf":
            return f" (RUNPATH) Library runpath: [$ORIGIN/../{self.libdir}]"
        return f"libTrophy.so => {self.prefix / self.libdir / 'libTrophy.so'} (0x1234)\n"

    def check(self):
        with patch.object(native, "command", side_effect=self.output):
            return native.check_tree(self.root)

    def test_prefix_with_spaces(self):
        self.payload()
        self.assertEqual(self.check(), self.prefix)

    def test_multilib_prefix(self):
        self.payload("lib/x86_64-linux-gnu")
        self.assertEqual(self.check(), self.prefix)

    def companions(self):
        directory = self.prefix / "lib/trophy/reporters32"
        directory.mkdir(parents=True)
        for name in ("libtrophy_layer.so", "libtrophy_gl_shim.so"):
            header = bytearray(20)
            header[:7] = b"\x7fELF\x01\x01\x01"
            header[16:20] = b"\x03\x00\x03\x00"
            (directory / name).write_bytes(header)
        return directory

    def test_optional_i386_companions_are_checked(self):
        self.payload()
        self.companions()
        self.assertEqual(self.check(), self.prefix)

    def test_partial_i386_companions_are_not_a_complete_package(self):
        self.payload()
        directory = self.companions()
        (directory / "libtrophy_layer.so").unlink()
        with self.assertRaisesRegex(RuntimeError, "Payload missing"):
            self.check()

    def test_mislabeled_i386_companion_is_rejected(self):
        self.payload()
        directory = self.companions()
        (directory / "libtrophy_gl_shim.so").write_bytes(b"not an i386 shared object")
        with self.assertRaisesRegex(RuntimeError, "i386 ET_DYN"):
            self.check()

    def test_missing_payload_members(self):
        self.payload()
        files = [p for p in self.prefix.rglob("*") if p.is_file()]
        for path in files:
            with self.subTest(path=path):
                data, mode = path.read_bytes(), path.stat().st_mode
                path.unlink()
                with self.assertRaises(RuntimeError):
                    self.check()
                path.write_bytes(data)
                path.chmod(mode)

    def test_shared_objects_need_read_not_execute_permissions(self):
        self.payload()
        for library in self.prefix.rglob("*.so"):
            library.chmod(0o644)
        self.assertEqual(self.check(), self.prefix)

    def test_unreadable_reporter(self):
        self.payload()
        (self.prefix / "lib/trophy/libtrophy_gl_shim.so").chmod(0o000)
        with self.assertRaisesRegex(RuntimeError, "permissions"):
            self.check()

    def test_nonexecutable_application(self):
        self.payload()
        (self.prefix / "bin/trophy").chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, "executable permissions"):
            self.check()

    def test_external_payload_symlink(self):
        self.payload()
        path = self.prefix / "lib/libTrophy.so"
        path.rename(self.root / "external.so")
        path.symlink_to(self.root / "external.so")
        with self.assertRaisesRegex(RuntimeError, "escapes prefix"):
            self.check()

    def test_wrong_launchable(self):
        self.payload()
        path = self.prefix / "share/metainfo/io.github.stevenjwheeler.Trophy.metainfo.xml"
        path.write_text('<component><launchable type="desktop-id">wrong.desktop</launchable></component>')
        with self.assertRaisesRegex(RuntimeError, "launchable"):
            self.check()

    def test_build_tree_runpath_rejected(self):
        self.payload()
        with patch.object(native, "command", return_value=" (RUNPATH) Library runpath: [/build]"):
            with self.assertRaisesRegex(RuntimeError, "relocatable RUNPATH"):
                native.check_tree(self.root)

    def test_malformed_manifest(self):
        self.payload()
        path = self.prefix / "share/trophy/vulkan_layer/trophy_layer.json"
        for manifest in ([], {"layer": []}, {"layer": {"library_path": {}}},
                         {"layer": {"library_path": " "}}):
            with self.subTest(manifest=manifest):
                path.write_text(json.dumps(manifest))
                with self.assertRaisesRegex(RuntimeError, "library_path"):
                    self.check()

    def test_host_library_cannot_mask_missing_local_resolution(self):
        self.payload()
        output = self.output
        with patch.object(native, "command", side_effect=lambda args:
                          output(args) if args[0] == "readelf" else
                          "libTrophy.so => /usr/lib/libTrophy.so (0x1234)"):
            with self.assertRaisesRegex(RuntimeError, "prefix-local"):
                native.check_tree(self.root)

    def test_missing_transitive_dependency(self):
        self.payload()
        output = self.output
        with patch.object(native, "command", side_effect=lambda args:
                          output(args) + ("libmissing.so => not found" if args[0] == "ldd" else "")):
            with self.assertRaisesRegex(RuntimeError, "Missing dependencies"):
                native.check_tree(self.root)

    def test_ambiguous_payload(self):
        self.payload()
        other = self.root / "other/bin/trophy"
        other.parent.mkdir(parents=True)
        other.write_text("fixture")
        with self.assertRaisesRegex(RuntimeError, "exactly one bin/trophy"):
            self.check()

    def test_linkage_commands_clear_loader_overrides(self):
        with patch.dict(os.environ, {"LD_LIBRARY_PATH": "/foreign", "LD_PRELOAD": "/foreign.so",
                                     "LD_AUDIT": "/audit.so", "LC_ALL": "foreign"}):
            with patch.object(native.subprocess, "run", return_value=SimpleNamespace(
                    returncode=0, stdout="output", stderr="")) as run:
                self.assertEqual(native.command(["ldd", "fixture"]), "output")
                env = run.call_args.kwargs["env"]
                for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
                    self.assertNotIn(key, env)
                self.assertEqual(env["LC_ALL"], "C")
                self.assertEqual(run.call_args.kwargs["timeout"], 30)

    def test_failed_inspection_is_not_a_pass(self):
        with patch.object(native.subprocess, "run", return_value=SimpleNamespace(
                returncode=1, stdout="", stderr="not a dynamic executable")):
            with self.assertRaisesRegex(RuntimeError, "ldd failed"):
                native.command(["ldd", "fixture"])

    def archive(self, name, kind=None, link=""):
        path = self.root / "package.tar.xz"
        with tarfile.open(path, "w:xz") as stream:
            member = tarfile.TarInfo(name)
            if kind:
                member.type, member.linkname = kind, link
                stream.addfile(member)
            else:
                member.size = 7
                stream.addfile(member, io.BytesIO(b"fixture"))
        return path

    def test_archive_regular_file(self):
        path = self.archive("package/usr/bin/trophy")
        native.extract_archive(path, self.root / "extract")
        self.assertEqual((self.root / "extract/package/usr/bin/trophy").read_text(), "fixture")

    def test_unsafe_archives(self):
        for name, kind, link in (
            ("../escape", None, ""), ("/absolute", None, ""),
            ("link", tarfile.SYMTYPE, "../../escape"),
            ("hardlink", tarfile.LNKTYPE, "/outside"),
            ("device", tarfile.CHRTYPE, ""),
        ):
            with self.subTest(name=name):
                path = self.archive(name, kind, link)
                with self.assertRaises((RuntimeError, tarfile.TarError)):
                    native.extract_archive(path, self.root / "extract")
        self.assertFalse((self.root / "escape").exists())


if __name__ == "__main__":
    unittest.main()
