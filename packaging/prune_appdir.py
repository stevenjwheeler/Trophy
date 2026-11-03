#!/usr/bin/env python3
"""Trim AppDir payload from an AppImage that Trophy can never load.

Run after linuxdeploy has deployed dependencies and before the image is
generated (CI and the diagnostic kit). Only payload that is provably unused is
removed; everything else stays, and missing required pieces fail loudly.

1. Duplicate libTrophy.so.
   `cmake --install` writes it into the multiarch directory and linuxdeploy
   deploys the used copy into usr/lib. The executable's RUNPATH is
   `$ORIGIN/../lib`, so the multiarch copy is never loaded.

2. Unused Qt Quick Controls styles.
   src/main.cpp calls QQuickStyle::setStyle("Fusion") unconditionally, which
   takes precedence over QT_QUICK_CONTROLS_STYLE, so Imagine/Material/
   Universal/FluentWinUI3 cannot be selected. QtQuick.Controls lists those as
   optional imports, while Fusion's qmldir imports QtQuick.Controls.Basic, so
   Basic and Fusion must stay.
"""

import argparse
import shutil
import sys
from pathlib import Path

REQUIRED_STYLES = ("Basic", "Fusion")
UNUSED_STYLES = ("Imagine", "Material", "Universal", "FluentWinUI3")


def _size_of(path):
    if path.is_dir() and not path.is_symlink():
        return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
    try:
        return path.stat().st_size
    except OSError:
        return 0


def _remove(paths, removed, total):
    for path in paths:
        if not (path.exists() or path.is_symlink()):
            continue
        total[0] += _size_of(path)
        removed.append(path)
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()


def prune(appdir):
    appdir = Path(appdir)
    lib = appdir / "usr/lib"
    if not appdir.is_dir():
        raise RuntimeError(f"not a directory: {appdir}")

    removed = []
    total = [0]

    # --- duplicate libTrophy.so -------------------------------------------
    trophies = sorted(p for p in lib.glob("**/libTrophy.so")
                      if p.is_file() and not p.is_symlink())
    if not trophies:
        raise RuntimeError("no libTrophy.so in the AppDir")
    keep = lib / "libTrophy.so"
    if keep in trophies:
        _remove([p for p in trophies if p != keep], removed, total)
    elif len(trophies) == 1:
        # Only the multiarch copy exists (linuxdeploy has not collapsed it yet):
        # move it to the path the executable's RUNPATH uses.
        trophies[0].rename(keep)
    if not keep.is_file():
        raise RuntimeError("libTrophy.so is not at usr/lib/libTrophy.so")
    if any(p != keep for p in lib.glob("**/libTrophy.so")):
        raise RuntimeError("duplicate libTrophy.so survived pruning")

    # --- Qt Quick Controls styles the app cannot select -------------------
    core = lib / "libQt6QuickControls2.so.6"
    if core.exists():
        for style in UNUSED_STYLES:
            _remove(sorted(lib.glob(f"libQt6QuickControls2{style}.so.6*")) +
                    sorted(lib.glob(f"libQt6QuickControls2{style}StyleImpl.so.6*")) +
                    [appdir / "usr/qml/QtQuick/Controls" / style],
                    removed, total)
        for style in REQUIRED_STYLES:
            if not any(lib.glob(f"libQt6QuickControls2{style}.so.6*")):
                raise RuntimeError(f"required Qt Quick Controls style missing: {style}")
            if not (appdir / "usr/qml/QtQuick/Controls" / style).is_dir():
                raise RuntimeError(f"required Qt Quick Controls QML module missing: {style}")
        for style in UNUSED_STYLES:
            if any(lib.glob(f"libQt6QuickControls2{style}*.so*")):
                raise RuntimeError(f"unused Qt Quick Controls style survived: {style}")

    return removed, total[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appdir", type=Path)
    args = parser.parse_args()
    try:
        removed, total = prune(args.appdir)
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"AppDir prune failed: {error}\n")
    print(f"Pruned {len(removed)} path(s), {total / 1048576:.1f} MiB uncompressed:")
    for path in removed:
        print(f"  - {path.relative_to(args.appdir)}")


if __name__ == "__main__":
    main()
