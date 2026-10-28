"""Draft (never publish) a newer stable release from verified CI packages."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from ci_version import read_version


def version(tag):
    match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", tag)
    return tuple(map(int, match.groups())) if match else None


def gh(*args):
    return subprocess.check_output(["gh", *args], text=True, timeout=180)


def api(path):
    return json.loads(gh("api", path))


def eligible(tag, releases):
    candidate = version(tag)
    if not tag.startswith("v") or candidate is None:
        return False
    published = []
    for release in releases:
        if release["draft"] or release["prerelease"]:
            continue
        parsed = version(release["tag_name"])
        if parsed is None:
            raise RuntimeError("Cannot compare published release tag: " + release["tag_name"])
        published.append(parsed)
    # Use the highest published stable version, even if GitHub's 'latest'
    # designation was manually moved backwards. Never draft a downgrade.
    return not published or candidate > max(published)


def on_main(sha):
    resolved = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True, timeout=30).strip()
    if resolved != sha:
        raise RuntimeError("Checkout no longer matches the tested commit")
    result = subprocess.run(["git", "merge-base", "--is-ancestor", resolved,
                             "refs/remotes/origin/main"], timeout=30)
    if result.returncode not in (0, 1):
        raise RuntimeError("Could not verify main ancestry")
    return result.returncode == 0


def remote_tag_sha(tag):
    ref = "refs/tags/" + tag
    output = subprocess.check_output(
        ["git", "ls-remote", "--tags", "origin", ref, ref + "^{}"],
        text=True, timeout=30)
    refs = {}
    for line in output.splitlines():
        value, name = line.split()
        refs[name] = value
    return refs.get(ref + "^{}", refs.get(ref))


def ensure_remote_tag(repo, tag, sha):
    current = remote_tag_sha(tag)
    if current is not None and current != sha:
        raise RuntimeError("Release tag already points to another commit; bump VERSION instead of moving it")
    if current is None:
        # A lightweight tag is created only after this main commit's complete
        # CI/package checks pass. No pre-existing tag is ever moved or deleted.
        gh("api", "--method", "POST", f"repos/{repo}/git/refs",
           "-f", "ref=refs/tags/" + tag, "-f", "sha=" + sha)
    if remote_tag_sha(tag) != sha:
        raise RuntimeError("Remote release tag changed; refusing assets from an older commit")


def prepare_assets(root, destination, tag):
    number = tag[1:]
    expected = {
        f"Trophy-{number}-x86_64.AppImage",
        f"dist/Trophy-{number}.tar.xz",
        f"dist/Trophy-{number}-amd64.deb",
        f"dist/Trophy-{number}-x86_64.rpm",
    }
    manifest = root / "SHA256SUMS"
    if manifest.is_symlink() or not manifest.is_file():
        raise RuntimeError("Missing regular checksum manifest")
    entries = {}
    for line in manifest.read_text().splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})  (.+)", line)
        if not match or match[2] not in expected or match[2] in entries:
            raise RuntimeError("Unexpected or duplicate checksum entry")
        entries[match[2]] = match[1].lower()
    if entries.keys() != expected:
        raise RuntimeError("Expected exactly four version-matched package checksums")
    for name, digest in entries.items():
        source = root / name
        if (source.is_symlink() or not source.is_file() or
                not source.resolve().is_relative_to(root.resolve())):
            raise RuntimeError("Missing or unsafe package: " + name)
        with source.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
                raise RuntimeError("Checksum mismatch: " + name)
    # Work only in a new temporary directory; flatten the CI dist/ layout so
    # release users can run sha256sum --check SHA256SUMS next to their downloads.
    destination.mkdir()
    hashes = {}
    for name, digest in sorted(entries.items()):
        filename = Path(name).name
        shutil.copyfile(root / name, destination / filename)
        hashes[filename] = digest
    checksums = destination / "SHA256SUMS"
    checksums.write_text("".join(f"{digest}  {name}\n" for name, digest in hashes.items()))
    hashes[checksums.name] = hashlib.sha256(checksums.read_bytes()).hexdigest()
    return hashes


def draft(repo, tag, sha, root):
    if (os.environ.get("GITHUB_EVENT_NAME") != "push" or
            os.environ.get("GITHUB_REF") != "refs/heads/main"):
        print("Not a push to main; skipping draft.")
        return
    if not tag.startswith("v") or version(tag) is None:
        print("Not a stable vX.Y.Z release tag; skipping draft.")
        return
    if not on_main(sha):
        print("Tested commit is not on main; skipping draft.")
        return
    endpoint = f"repos/{repo}/releases"
    pages = json.loads(gh("api", "--paginate", "--slurp", endpoint + "?per_page=100"))
    releases = [release for page in pages for release in page]
    if not eligible(tag, releases):
        print("Tag is not newer than every published stable release; skipping draft.")
        return
    existing = next((release for release in releases if release["tag_name"] == tag), None)
    if existing is not None and not existing["draft"]:
        print("Tag already has a published release; leaving it unchanged.")
        return
    with tempfile.TemporaryDirectory(prefix="trophy-draft-", dir=os.environ.get("RUNNER_TEMP")) as directory:
        staged = Path(directory) / "assets"
        hashes = prepare_assets(root, staged, tag)
        # Recheck the published baseline immediately before any write.
        fresh = json.loads(gh("api", "--paginate", "--slurp", endpoint + "?per_page=100"))
        if not eligible(tag, [release for page in fresh for release in page]):
            print("Published baseline changed; skipping draft.")
            return
        ensure_remote_tag(repo, tag, sha)
        if existing is None:
            gh("release", "create", tag, "--repo", repo, "--verify-tag", "--draft",
               "--target", "main", "--title", "Trophy " + tag, "--generate-notes")
        if existing is None:
            pages = json.loads(gh("api", "--paginate", "--slurp", endpoint + "?per_page=100"))
            existing = next((release for page in pages for release in page
                             if release["tag_name"] == tag), None)
            if existing is None:
                raise RuntimeError("Created draft was not returned by the releases API")
        release = api(endpoint + f"/{existing['id']}")
        if not release["draft"] or release["tag_name"] != tag or release["prerelease"]:
            raise RuntimeError("Refusing to modify a published or prerelease release")
        assets = api(endpoint + f"/{release['id']}/assets?per_page=100")
        uploads = []
        for name, digest in hashes.items():
            matches = [asset for asset in assets if asset["name"] == name]
            if not matches:
                uploads.append((staged / name, False))
            elif len(matches) != 1:
                raise RuntimeError("Duplicate draft assets: " + name)
            elif matches[0].get("digest") != "sha256:" + digest:
                uploads.append((staged / name, True))
        # hashes keeps SHA256SUMS last: refresh it only after all package bytes.
        # Failed uploads leave an unpublished draft that a later run can repair.
        for path, replace in uploads:
            if not api(endpoint + f"/{release['id']}")["draft"]:
                raise RuntimeError("Release was published during preparation; refusing upload")
            options = ("--clobber",) if replace else ()
            gh("release", "upload", tag, str(path), "--repo", repo, *options)
        attached = api(endpoint + f"/{release['id']}/assets?per_page=100")
        for name, digest in hashes.items():
            matches = [asset for asset in attached if asset["name"] == name]
            if len(matches) != 1 or matches[0].get("digest") != "sha256:" + digest:
                raise RuntimeError("Draft upload verification failed: " + name)
        print("Draft ready for human review; publication is never performed by CI.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--version-file", required=True, type=Path)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--assets", required=True, type=Path)
    args = parser.parse_args()
    draft(args.repo, "v" + read_version(args.version_file), args.sha, args.assets.resolve())
