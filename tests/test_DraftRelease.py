"""Release safety fixtures: mocked GitHub API and isolated temporary git repos."""

import contextlib
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "draft_release", Path(__file__).parents[1] / "packaging/draft_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def entry(tag, draft=False, prerelease=False, id=1):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease, "id": id}


class DraftReleaseTest(unittest.TestCase):
    def setUp(self):
        root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.temp = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.assets = self.root / "input"
        (self.assets / "dist").mkdir(parents=True)
        self.names = ["Trophy-1.2.3-x86_64.AppImage", "dist/Trophy-1.2.3.tar.xz",
                      "dist/Trophy-1.2.3-amd64.deb", "dist/Trophy-1.2.3-x86_64.rpm"]
        lines = []
        for name in self.names:
            data = name.encode()
            (self.assets / name).write_bytes(data)
            lines.append(hashlib.sha256(data).hexdigest() + "  " + name + "\n")
        (self.assets / "SHA256SUMS").write_text("".join(lines))
        self.releases = [entry("v1.2.2")]
        self.attached = []
        self.calls = []
        self.publish_on_guard = False
        self.publish_after_upload = False

    def fake_gh(self, *args):
        self.calls.append(args)
        if args[:3] == ("api", "--paginate", "--slurp"):
            return json.dumps([self.releases])
        if args[:2] == ("release", "create"):
            self.assertIn("--draft", args)
            self.assertIn("--verify-tag", args)
            self.assertEqual(args[args.index("--target") + 1], "main")
            self.releases.append(entry("v1.2.3", draft=True, id=2))
            return "draft URL"
        if args[:2] == ("release", "upload"):
            for name in args[3:args.index("--repo")]:
                path = Path(name)
                existing = [asset for asset in self.attached if asset["name"] == path.name]
                if existing:
                    self.assertIn("--clobber", args)
                    self.attached = [asset for asset in self.attached if asset["name"] != path.name]
                else:
                    self.assertNotIn("--clobber", args)
                self.attached.append({"name": path.name,
                                      "digest": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()})
            if self.publish_after_upload:
                next(item for item in self.releases if item["id"] == 2)["draft"] = False
            return ""
        if args[0] == "api" and "/assets?" in args[1]:
            return json.dumps(self.attached)
        if args[0] == "api":
            target = next(item for item in self.releases if item["id"] == 2)
            if self.publish_on_guard:
                self.publish_on_guard = False
                result = dict(target, draft=False)
                return json.dumps(result)
            return json.dumps(target)
        self.fail("Unexpected GitHub operation: " + repr(args))

    def run_draft(self):
        with patch.object(release, "on_main", return_value=True), \
                patch.object(release, "verify_remote_tag"), \
                patch.object(release, "gh", side_effect=self.fake_gh):
            release.draft("owner/repo", "v1.2.3", "tested-sha", self.assets)

    def test_version_gate_numeric_not_lexical(self):
        self.assertTrue(release.eligible("v1.10.0", [entry("v1.9.9")]))
        for tag in ("v1.2.2", "v1.1.9", "v0.9.0"):
            self.assertFalse(release.eligible(tag, self.releases))

    def test_first_release_and_ignore_drafts_prereleases(self):
        self.assertTrue(release.eligible("v1.2.3", []))
        self.assertTrue(release.eligible("v1.2.3", [entry("v9.0.0", draft=True),
                                                              entry("v8.0.0-rc1", prerelease=True)]))

    def test_rejects_nonstable_tags_and_ambiguous_published_versions(self):
        for tag in ("1.2.3", "v1.2.3-rc1", "v1.2.3-dev-sha", "v01.2.3", "v1.2"):
            self.assertFalse(release.eligible(tag, []))
        with self.assertRaisesRegex(RuntimeError, "Cannot compare"):
            release.eligible("v1.2.3", [entry("legacy-release")])

    def test_compares_highest_published_even_if_list_unsorted(self):
        self.assertFalse(release.eligible("v1.2.3", [entry("v0.9.0"), entry("v2.0.0")]))

    def test_nonmain_tag_never_contacts_github(self):
        with patch.object(release, "on_main", return_value=False), patch.object(release, "gh") as gh:
            release.draft("owner/repo", "v1.2.3", "sha", self.assets)
            gh.assert_not_called()

    def test_nonrelease_tag_never_checks_git_or_github(self):
        with patch.object(release, "on_main") as git, patch.object(release, "gh") as gh:
            release.draft("owner/repo", "v1.2.3-rc1", "sha", self.assets)
            git.assert_not_called()
            gh.assert_not_called()

    def test_git_requires_tested_tag_commit_and_main_ancestry(self):
        with patch.object(release.subprocess, "check_output", return_value="tested\n"), \
                patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            self.assertTrue(release.on_main("v1.2.3", "tested"))
            self.assertEqual(run.call_args.args[0][-1], "refs/remotes/origin/main")
            with self.assertRaisesRegex(RuntimeError, "tested commit"):
                release.on_main("v1.2.3", "different")

    def test_git_nonmain_and_failure_are_distinguished(self):
        with patch.object(release.subprocess, "check_output", return_value="tested\n"), \
                patch.object(release.subprocess, "run") as run:
            run.return_value.returncode = 1
            self.assertFalse(release.on_main("v1.2.3", "tested"))
            run.return_value.returncode = 128
            with self.assertRaisesRegex(RuntimeError, "ancestry"):
                release.on_main("v1.2.3", "tested")

    def test_remote_annotated_lightweight_and_retargeted_tags(self):
        ref = "refs/tags/v1.2.3"
        for output in (f"tested\t{ref}\n", f"tag-object\t{ref}\ntested\t{ref}^{{}}\n"):
            with patch.object(release.subprocess, "check_output", return_value=output):
                release.verify_remote_tag("v1.2.3", "tested")
        for output in ("", f"new-commit\t{ref}\n"):
            with patch.object(release.subprocess, "check_output", return_value=output):
                with self.assertRaisesRegex(RuntimeError, "Remote release tag changed"):
                    release.verify_remote_tag("v1.2.3", "tested")

    def test_retargeted_remote_tag_prevents_github_writes(self):
        with patch.object(release, "on_main", return_value=True), \
                patch.object(release, "verify_remote_tag", side_effect=RuntimeError("tag changed")), \
                patch.object(release, "gh", side_effect=self.fake_gh):
            with self.assertRaisesRegex(RuntimeError, "tag changed"):
                release.draft("owner/repo", "v1.2.3", "sha", self.assets)
        self.assertTrue(all(call[0] == "api" for call in self.calls))

    def test_real_annotated_tags_must_belong_to_main(self):
        repo = self.root / "repository"
        repo.mkdir()

        def git(*args):
            return subprocess.check_output(
                ["git", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
                 "-c", "user.name=Release Fixture", "-c", "user.email=fixture@example.invalid",
                 *args], cwd=repo, text=True, stderr=subprocess.DEVNULL).strip()

        git("init", "-b", "main")
        git("commit", "--allow-empty", "-m", "Main fixture")
        sha = git("rev-parse", "HEAD")
        git("update-ref", "refs/remotes/origin/main", sha)
        git("tag", "-a", "v1.2.3", "-m", "Release fixture")
        with contextlib.chdir(repo):
            self.assertTrue(release.on_main("v1.2.3", sha))
            with self.assertRaisesRegex(RuntimeError, "tested commit"):
                release.on_main("v1.2.3", "0" * 40)
        git("checkout", "-b", "dev")
        git("commit", "--allow-empty", "-m", "Unmerged fixture")
        dev_sha = git("rev-parse", "HEAD")
        git("tag", "-a", "v1.2.4", "-m", "Unmerged tag")
        with contextlib.chdir(repo):
            self.assertFalse(release.on_main("v1.2.4", dev_sha))

    def test_published_release_is_never_written(self):
        self.releases.append(entry("v1.2.3", id=2))
        self.run_draft()
        self.assertTrue(all(call[0] == "api" for call in self.calls))

    def test_flat_manifest_has_exact_files_and_verifiable_hashes(self):
        output = self.root / "flat"
        hashes = release.prepare_assets(self.assets, output, "v1.2.3")
        self.assertEqual(len(hashes), 5)
        self.assertNotIn("dist/", (output / "SHA256SUMS").read_text())
        for line in (output / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ")
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)

    def test_corrupt_package_fails_before_creating_draft(self):
        (self.assets / self.names[0]).write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
            self.run_draft()
        self.assertTrue(all(call[0] == "api" for call in self.calls))

    def test_rejects_missing_extra_duplicate_and_traversal_manifest_entries(self):
        manifest = self.assets / "SHA256SUMS"
        original = manifest.read_text()
        for text in ("", original + original.splitlines()[0] + "\n",
                     original.replace(self.names[0], "../outside"),
                     original.replace(self.names[0], "Trophy-9.0.0-x86_64.AppImage")):
            with self.subTest(text=text):
                manifest.write_text(text)
                with self.assertRaises(RuntimeError):
                    release.prepare_assets(self.assets, self.root / "flat", "v1.2.3")

    def test_symlink_payload_is_rejected(self):
        path = self.assets / self.names[0]
        path.unlink()
        path.symlink_to(self.assets / self.names[1])
        with self.assertRaisesRegex(RuntimeError, "unsafe package"):
            release.prepare_assets(self.assets, self.root / "flat", "v1.2.3")

    def test_creates_draft_only_and_attaches_five_verified_files(self):
        self.run_draft()
        self.assertEqual(len(self.attached), 5)
        writes = [call for call in self.calls if call[0] != "api"]
        self.assertEqual([call[1] for call in writes], ["create"] + ["upload"] * 5)
        self.assertEqual(Path(writes[-1][3]).name, "SHA256SUMS")

    def test_existing_draft_retry_is_idempotent_and_keeps_notes(self):
        self.run_draft()
        self.calls.clear()
        self.run_draft()
        self.assertTrue(all(call[0] == "api" for call in self.calls))

    def test_partial_draft_uploads_only_missing_files(self):
        self.run_draft()
        self.attached.pop()
        self.calls.clear()
        self.run_draft()
        writes = [call for call in self.calls if call[0] != "api"]
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][1], "upload")
        self.assertEqual(len(writes[0][3:writes[0].index("--repo")]), 1)

    def test_changed_draft_asset_is_replaced_without_editing_notes(self):
        self.run_draft()
        self.attached[0]["digest"] = "sha256:" + "0" * 64
        self.calls.clear()
        self.run_draft()
        writes = [call for call in self.calls if call[0] != "api"]
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][1], "upload")
        self.assertIn("--clobber", writes[0])

    def test_rebuilt_package_refreshes_asset_then_checksums_and_preserves_manual_notes(self):
        self.run_draft()
        target = next(item for item in self.releases if item["id"] == 2)
        target.update(name="Edited release title", body="My hand-written release notes")
        path = self.assets / self.names[0]
        old_digest = hashlib.sha256(path.read_bytes()).hexdigest()
        path.write_bytes(b"new successful CI package bytes")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest = self.assets / "SHA256SUMS"
        manifest.write_text(manifest.read_text().replace(old_digest, digest))
        self.calls.clear()
        self.run_draft()
        writes = [call for call in self.calls if call[0] != "api"]
        self.assertEqual([Path(call[3]).name for call in writes],
                         [Path(self.names[0]).name, "SHA256SUMS"])
        self.assertTrue(all("--clobber" in call for call in writes))
        self.assertEqual(target["name"], "Edited release title")
        self.assertEqual(target["body"], "My hand-written release notes")

    def test_publication_between_uploads_stops_remaining_writes(self):
        self.publish_after_upload = True
        with self.assertRaisesRegex(RuntimeError, "published"):
            self.run_draft()
        uploads = [call for call in self.calls if call[:2] == ("release", "upload")]
        self.assertEqual(len(uploads), 1)
        self.assertNotEqual(Path(uploads[0][3]).name, "SHA256SUMS")

    def test_release_published_during_preparation_is_not_uploaded(self):
        self.releases.append(entry("v1.2.3", draft=True, id=2))
        self.publish_on_guard = True
        with self.assertRaisesRegex(RuntimeError, "published"):
            self.run_draft()
        self.assertTrue(all(call[0] == "api" for call in self.calls))

    def test_newer_release_published_before_create_prevents_all_writes(self):
        with patch.object(release, "on_main", return_value=True), \
                patch.object(release, "gh", side_effect=[json.dumps([self.releases]),
                                                       json.dumps([[entry("v2.0.0")]])]) as gh:
            release.draft("owner/repo", "v1.2.3", "sha", self.assets)
            self.assertEqual(gh.call_count, 2)
            self.assertTrue(all(call.args[0] == "api" for call in gh.call_args_list))


if __name__ == "__main__":
    unittest.main()
