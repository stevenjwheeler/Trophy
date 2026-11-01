"""Check a trusted Trophy native prefix or tar.xz on the build host, without installing it.

This executes ldd and the packaged application: do not use with untrusted packages.
It does not prove package-manager dependencies or compatibility on another distro.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET

from smoke_appimage import smoke


def command(arguments):
    env = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
        env.pop(key, None)
    env["LC_ALL"] = "C"
    result = subprocess.run(arguments, env=env, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"{arguments[0]} failed for {arguments[-1]}: {result.stdout}{result.stderr}")
    return result.stdout


def required(prefix, relative, executable=False):
    path = prefix / relative
    if not path.resolve().is_relative_to(prefix.resolve()):
        raise RuntimeError(f"Payload escapes prefix: {relative}")
    if not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"Payload missing or empty: {relative}")
    if not path.stat().st_mode & 0o444:
        raise RuntimeError(f"Payload lacks readable permissions: {relative}")
    if executable and path.stat().st_mode & 0o111 != 0o111:
        raise RuntimeError(f"Payload lacks executable permissions: {relative}")
    return path


def check_tree(root):
    # Accept a prefix, DESTDIR tree, or CPack's single enclosing directory.
    binaries = list(root.rglob("bin/trophy"))
    if len(binaries) != 1:
        raise RuntimeError(f"Expected exactly one bin/trophy, found {len(binaries)}")
    prefix = binaries[0].parent.parent
    binary = required(prefix, "bin/trophy", executable=True)
    libraries = list(prefix.glob("lib*/**/libTrophy.so"))
    if len(libraries) != 1:
        raise RuntimeError(f"Expected exactly one libTrophy.so, found {len(libraries)}")
    libdir = libraries[0].parent.relative_to(prefix)
    # Shared objects need read access, not filesystem execute bits. Debian's
    # Qt/CMake install policy legitimately installs libraries as 0644.
    elfs = [binary, required(prefix, libdir / "libTrophy.so")]
    elfs += [required(prefix, libdir / "trophy" / name)
             for name in ("libtrophy_layer.so", "libtrophy_gl_shim.so")]
    companion_dir = prefix / "lib/trophy/reporters32"
    if companion_dir.exists() or companion_dir.is_symlink():
        if companion_dir.is_symlink() or not companion_dir.is_dir():
            raise RuntimeError("Invalid i386 companion directory")
        for name in ("libtrophy_layer.so", "libtrophy_gl_shim.so"):
            companion = required(prefix, Path("lib/trophy/reporters32") / name)
            with companion.open("rb") as file:
                header = file.read(20)
            if (len(header) != 20 or header[:7] != b"\x7fELF\x01\x01\x01" or
                    header[16:20] != b"\x03\x00\x03\x00" or companion.is_symlink()):
                raise RuntimeError(f"Expected regular i386 ET_DYN companion: {companion}")
            elfs.append(companion)
    for relative in (
        "share/applications/io.github.stevenjwheeler.Trophy.desktop",
        "share/icons/hicolor/scalable/apps/trophy.svg",
        "share/trophy/vulkan_layer/trophy_layer.json",
        "share/trophy/kwin/trophy-monitor/metadata.json",
        "share/trophy/kwin/trophy-monitor/contents/code/main.js",
        "share/licenses/trophy/default-app-icons/LICENSE",
        "share/licenses/trophy/default-app-icons/README.md",
    ):
        required(prefix, relative)
    metainfo = required(prefix, "share/metainfo/io.github.stevenjwheeler.Trophy.metainfo.xml")
    launchables = ET.parse(metainfo).getroot().findall("launchable")
    expected_launchable = "io.github.stevenjwheeler.Trophy.desktop"
    if not any(item.get("type") == "desktop-id" and item.text == expected_launchable
               for item in launchables):
        raise RuntimeError(f"AppStream launchable must name {expected_launchable}")
    manifest = json.loads((prefix / "share/trophy/vulkan_layer/trophy_layer.json").read_text())
    layer = manifest.get("layer") if isinstance(manifest, dict) else None
    library_path = layer.get("library_path") if isinstance(layer, dict) else None
    if not isinstance(library_path, str) or not library_path.strip():
        raise RuntimeError("Vulkan manifest lacks library_path")
    # The manifest is an enrollment template; its build-tree path is rewritten
    # by explicit enrollment. Do not mistake it for an installed loader path.
    dynamic = command(["readelf", "-d", str(binary)])
    paths = re.findall(r"\((?:RUNPATH|RPATH)\).*?\[(.*?)\]", dynamic)
    expected = f"$ORIGIN/../{libdir}"
    if paths != [expected]:
        raise RuntimeError(f"Expected only relocatable RUNPATH {expected}, got {paths}")
    for elf in elfs:
        output = command(["ldd", str(elf)])
        if "not found" in output:
            raise RuntimeError(f"Missing dependencies for {elf}: {output}")
        if elf == binary:
            match = re.search(r"libTrophy\.so => (.+?) \(", output)
            if not match or Path(match[1]).resolve() != libraries[0].resolve():
                raise RuntimeError("Executable does not resolve prefix-local libTrophy.so")
    return prefix


def extract_archive(archive, destination):
    # Refuse traversal, escaping links, devices and oversized archives before
    # writing anything. Python's data filter is required (3.12+ or backport).
    if not hasattr(tarfile, "data_filter"):
        raise RuntimeError("Archive inspection requires Python tarfile.data_filter")
    with tarfile.open(archive, "r:xz") as stream:
        members = stream.getmembers()
        if len(members) > 100000 or sum(member.size for member in members) > 8 * 1024**3:
            raise RuntimeError("Archive exceeds smoke-test extraction budget")
        for member in members:
            if member.name.startswith("/") or ".." in Path(member.name).parts:
                raise RuntimeError(f"Unsafe archive path: {member.name}")
            tarfile.data_filter(member, str(destination))
        stream.extractall(destination, members=members, filter="data")


def verify(root, log):
    prefix = check_tree(root)
    # Native prefixes link the host's Qt/FFmpeg by design.
    smoke(prefix / "bin/trophy", log, expect_bundled_runtime=False)
    print(f"Native payload/linkage/startup passed: {prefix}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="extracted prefix/DESTDIR or trusted .tar.xz")
    parser.add_argument("--log", type=Path, default=Path("native-smoke.log"))
    args = parser.parse_args()
    try:
        if args.package.is_dir():
            verify(args.package.absolute(), args.log.resolve())
        else:
            temp_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
            # A spaced extraction path exercises relocation rather than only
            # the build's original prefix; keep extraction alive through smoke.
            with tempfile.TemporaryDirectory(prefix="trophy native package ", dir=temp_root) as directory:
                root = Path(directory)
                extract_archive(args.package, root)
                verify(root, args.log.resolve())
    except (OSError, ValueError, RuntimeError, tarfile.TarError, ET.ParseError,
            subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Native smoke failed: {error}; log: {args.log}\n")
