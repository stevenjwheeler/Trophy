"""Inspect standalone reporter outputs without contacting a graphics driver."""
import argparse
import json
import re
from pathlib import Path
import subprocess


def check(directory, bits):
    name = "libtrophy_layer.so"
    path = directory / name
    header = path.read_bytes()[:20]
    if header[:4] != b"\x7fELF" or header[4] != (1 if bits == 32 else 2):
        raise RuntimeError(f"Wrong ELF class for {path}")
    if not re.search(rb"TROPHY_HELPER_VERSION:vulkan:[1-9][0-9]{0,8}\x00", path.read_bytes()):
        raise RuntimeError(f"Missing integer helper version in {path}")
    # The i686 toolchain must really produce i386, not x32 ABI or a
    # mislabeled copy of the host ELF. Other native 64-bit machines retain
    # their architecture; no x86_64-only claim is made for those builds.
    if bits == 32 and (header[5] != 1 or int.from_bytes(header[18:20], "little") != 3):
        raise RuntimeError(f"Expected little-endian i386 ELF for {path}")
    dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True, timeout=30)
    if "libvulkan.so" in dynamic or "libQt" in dynamic or "libavcodec" in dynamic:
        raise RuntimeError(f"Reporter has an unwanted loader/application dependency: {path}")
    document = json.loads((directory / "trophy_layer.json").read_text())
    if document["file_format_version"] != "1.2.1":
        raise RuntimeError("Architecture filtering requires manifest format 1.2.1")
    manifest = document["layer"]
    if manifest["library_arch"] != str(bits) or manifest["library_path"] != "./libtrophy_layer.so":
        raise RuntimeError("Manifest architecture/path does not match standalone reporter")
    if manifest["disable_environment"] != {"DISABLE_TROPHY_DETECTION": "1"}:
        raise RuntimeError("Reporter opt-out changed")
    print(f"{bits}-bit reporter ELF/dependency/manifest checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bits", type=int, choices=(32, 64), required=True)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    check(args.directory, args.bits)
