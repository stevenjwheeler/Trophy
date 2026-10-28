#!/usr/bin/env python3
"""Copy a bounded ABI-provider subset from a trusted Steam runtime into scratch.

Steam's content store is read-only input. Hashes and sizes from usr-mtree are
verified; no Steam script is executed and no runtime enrollment is changed.
The result is a provider subset, not a runnable reconstructed Steam runtime.
"""
import argparse
import gzip
import hashlib
import json
import posixpath
from pathlib import Path
import re
import shlex

NAMES = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
         "libstdc++.so.6", "libgcc_s.so.1", "libvulkan.so.1",
         "ld-linux.so.2", "ld-linux-x86-64.so.2"}


def entries(manifest):
    result = {}
    with gzip.open(manifest, "rt") as stream:
        for count, line in enumerate(stream):
            if count >= 200000 or len(line) > 16384:
                raise ValueError("mtree exceeds bounded input budget")
            if line.startswith("#") or not line.strip():
                continue
            words = shlex.split(line)
            name = words[0].removeprefix("./")
            if name == ".":
                continue
            if name.startswith("/") or ".." in Path(name).parts:
                raise ValueError("unsafe mtree path")
            result[name] = dict(word.split("=", 1) for word in words[1:] if "=" in word)
    return result


def resolve(name, table):
    for _ in range(16):
        row = table.get(name, {})
        if row.get("type") != "link":
            return name, row
        link = row.get("link", "")
        name = posixpath.normpath(link.lstrip("/") if link.startswith("/") else
                                 posixpath.join(posixpath.dirname(name), link))
        # usr-mtree describes /usr; merged-/usr root library aliases refer back
        # into this same tree, never to the host filesystem.
        name = name.removeprefix("usr/")
        if name.startswith("../") or name == "..":
            raise ValueError("mtree symlink escapes provider tree")
    raise ValueError("mtree symlink chain exceeds bound")


def materialize(runtime, destination):
    if destination.exists():
        raise ValueError("destination must be a new scratch directory")
    table = entries(runtime / "usr-mtree.txt.gz")
    targets = {}
    for name in table:
        if Path(name).name in NAMES:
            target, row = resolve(name, table)
            if row.get("type") == "file":
                size = int(row.get("size", "0"))
                digest = row.get("sha256", "")
                if not 0 < size <= 32 * 1024 * 1024 or not re.fullmatch("[0-9a-f]{64}", digest):
                    raise ValueError("invalid provider size/hash")
                targets[name] = (size, digest)
    if not targets:
        raise ValueError("no supported ABI providers in manifest")
    sizes = {size for size, _ in targets.values()}
    wanted = {digest for _, digest in targets.values()}
    found, hashed = {}, 0
    # The store's filenames are not assumed to be SHA256s. Select candidates
    # by bounded recorded size, then verify their complete content digest.
    files = runtime / "files"
    if files.is_symlink() or not files.is_dir():
        raise ValueError("runtime has no regular content store")
    count = 0
    for bucket in sorted(files.iterdir()):
        if bucket.is_symlink() or not bucket.is_dir():
            continue
        for path in sorted(bucket.iterdir()):
            count += 1
            if count > 100000:
                raise ValueError("runtime store exceeds file budget")
            if path.is_symlink() or not path.is_file() or path.stat().st_size not in sizes:
                continue
            data = path.read_bytes()
            hashed += len(data)
            if hashed > 512 * 1024 * 1024:
                raise ValueError("runtime hashing exceeds byte budget")
            digest = hashlib.sha256(data).hexdigest()
            if digest in wanted and digest not in found:
                if not data.startswith(b"\x7fELF"):
                    raise ValueError("provider content is not ELF")
                found[digest] = path
    destination.mkdir(parents=True)
    copied, missing = [], []
    for name, (_, digest) in targets.items():
        if digest not in found:
            missing.append(name)
            continue
        target = destination / "usr" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        data = found[digest].read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("provider changed during materialization")
        target.write_bytes(data)
        target.chmod(0o755 if target.name.startswith("ld-") else 0o644)
        copied.append({"path": name, "sha256": digest, "source": str(found[digest])})
    report = {"scope": "hash-verified core ABI provider subset, not a complete Steam runtime",
              "runtime": str(runtime), "copied": copied, "missing": missing}
    (destination / "provenance.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    result = materialize(args.runtime, args.destination)
    print(f"Copied {len(result['copied'])} verified providers; {len(result['missing'])} unavailable")
