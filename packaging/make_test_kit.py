"""Stage a portable diagnostics test kit from a completed Trophy build.

The kit contains the collection script, the build's isolated test binaries and
libTrophy.so, plus a short README. It is meant to be extracted on a machine
with a compatible Qt/FFmpeg installation (the same baseline the build used);
it does not bundle Qt/FFmpeg the way the retired AppImage diagnostic bundle did.

Usage:
    python3 packaging/make_test_kit.py --build /path/to/build --output /tmp/trophy-test-kit
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block) if False else h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, help="Completed BUILD_TESTING=ON build directory")
    parser.add_argument("--appimage", type=Path, help="Diagnostic AppImage containing embedded tests")
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    parser.add_argument("--version", default="diagnostics", help="Label for the kit")
    args = parser.parse_args()

    source = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output == source or source in output.parents or output.exists():
        parser.error("Output must be a new directory outside the source tree")
    build = args.build.resolve() if args.build else None
    if build is not None and not (build / "tests").is_dir():
        parser.error("Build directory has no tests/ subdirectory: " + str(build))
    if build is None and args.appimage is None:
        parser.error("Provide --build and/or --appimage")

    output.mkdir(parents=True, mode=0o700)
    kit = output / "kit"
    kit.mkdir(mode=0o700)
    (kit / "tests").mkdir(mode=0o700)

    # Collection script
    shutil.copyfile(source / "packaging" / "collect_diagnostics.py", kit / "collect_diagnostics.py")

    # Test binaries from build
    count = 0
    if build is not None:
        for binary in sorted((build / "tests").iterdir()):
            if binary.is_file() and binary.name.startswith("test_"):
                shutil.copyfile(binary, kit / "tests" / binary.name)
                (kit / "tests" / binary.name).chmod(0o755)
                count += 1
        lib = build / "libTrophy.so"
        if lib.is_file():
            shutil.copyfile(lib, kit / "libTrophy.so")
        for name in ("trophy",):
            p = build / name
            if p.is_file():
                shutil.copyfile(p, kit / name)
                (kit / name).chmod(0o755)

    # Diagnostic AppImage
    if args.appimage is not None:
        img = args.appimage.resolve()
        if not img.is_file():
            parser.error("AppImage not found: " + str(img))
        shutil.copyfile(img, kit / "Trophy.AppImage")
        (kit / "Trophy.AppImage").chmod(0o755)

    has_appimage = (kit / "Trophy.AppImage").is_file()
    has_build = (kit / "tests").exists() and any((kit / "tests").iterdir())

    lines = ["Trophy diagnostics test kit", "===========================", ""]
    if has_appimage:
        lines += [
            "This kit includes a diagnostic AppImage with embedded tests.",
            "No host Qt/FFmpeg installation is required to run them.",
            "",
            "Run the collection script:",
            "    python3 collect_diagnostics.py --appimage ./Trophy.AppImage",
            "",
        ]
    if has_build:
        lines += [
            "This kit also contains build test binaries (requires compatible host Qt/FFmpeg).",
            "Run the collection script:",
            "    python3 collect_diagnostics.py --build .",
            "",
            "Or run the whole suite directly:",
            "    ctest --test-dir . --output-on-failure",
            "",
        ]
    lines += [
        "The script gathers system info, your Trophy log (if present) and test",
        "results into a redacted tar.gz for examination.",
    ]
    (kit / "README.txt").write_text("\n".join(lines) + "\n")

    if has_appimage:
        (kit / "run.sh").write_text("#!/bin/sh\nexec python3 \"$(dirname \"$0\")/collect_diagnostics.py\" --appimage \"$(dirname \"$0\")/Trophy.AppImage\" \"$@\"\n")
    else:
        (kit / "run.sh").write_text("#!/bin/sh\nexec python3 \"$(dirname \"$0\")/collect_diagnostics.py\" --build \"$(dirname \"$0\")\" \"$@\"\n")
    (kit / "run.sh").chmod(0o755)

    print("Kit staged:", kit)
    print("Test binaries:", count)
    print("Run: cd", kit, "&& ./run.sh")


if __name__ == "__main__":
    sys.exit(main())
