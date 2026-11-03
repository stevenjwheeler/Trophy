"""Checks for the AppDir pruning done before AppImage generation."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "trophy_prune", Path(__file__).parents[1] / "packaging/prune_appdir.py")
prune_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prune_module)


class PruneAppDirTest(unittest.TestCase):
    def setUp(self):
        root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="trophy-prune-test-", dir=root)
        self.addCleanup(self.directory.cleanup)
        self.appdir = Path(self.directory.name) / "AppDir"
        self.lib = self.appdir / "usr/lib"

    def build(self, *, nested_trophy=True, styles=("Basic", "Fusion", "Imagine", "Material")):
        self.lib.mkdir(parents=True, exist_ok=True)
        (self.lib / "libTrophy.so").write_bytes(b"lib")
        if nested_trophy:
            (self.lib / "x86_64-linux-gnu").mkdir()
            (self.lib / "x86_64-linux-gnu/libTrophy.so").write_bytes(b"lib")
        (self.lib / "libQt6QuickControls2.so.6").write_bytes(b"core")
        for style in styles:
            (self.lib / f"libQt6QuickControls2{style}.so.6").write_bytes(b"style")
            module = self.appdir / "usr/qml/QtQuick/Controls" / style
            module.mkdir(parents=True, exist_ok=True)
            (module / "qmldir").write_text("module\n")
        return self.appdir

    def test_duplicate_library_and_unused_styles_are_removed(self):
        removed, size = prune_module.prune(
            self.build(styles=("Basic", "Fusion", "Imagine", "Material")))
        self.assertGreater(size, 0)
        self.assertTrue(self.lib.joinpath("libTrophy.so").is_file())
        self.assertFalse(self.lib.joinpath("x86_64-linux-gnu/libTrophy.so").exists())
        for style in ("Imagine", "Material"):
            self.assertFalse(self.lib.joinpath(f"libQt6QuickControls2{style}.so.6").exists())
            self.assertFalse(
                (self.appdir / "usr/qml/QtQuick/Controls" / style).exists())
        for style in ("Basic", "Fusion"):
            self.assertTrue(self.lib.joinpath(f"libQt6QuickControls2{style}.so.6").is_file())
            self.assertTrue((self.appdir / "usr/qml/QtQuick/Controls" / style).is_dir())
        # nested libTrophy + 2 unused style libs + 2 unused QML modules
        self.assertEqual(len(removed), 5)

    def test_single_library_is_kept(self):
        prune_module.prune(self.build(nested_trophy=False))
        self.assertTrue(self.lib.joinpath("libTrophy.so").is_file())

    def test_missing_required_style_fails(self):
        with self.assertRaisesRegex(RuntimeError, "required Qt Quick Controls style missing"):
            prune_module.prune(self.build(styles=("Imagine",)))

    def test_missing_library_fails(self):
        self.build()
        self.lib.joinpath("libTrophy.so").unlink()
        self.lib.joinpath("x86_64-linux-gnu/libTrophy.so").unlink()
        with self.assertRaisesRegex(RuntimeError, "no libTrophy.so"):
            prune_module.prune(self.appdir)


if __name__ == "__main__":
    unittest.main()
