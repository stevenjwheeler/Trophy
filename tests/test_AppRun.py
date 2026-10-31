"""Launcher fixtures for packaging/AppRun and packaging/DiagnosticAppRun.

The libva host override is decided entirely in shell, and getting it wrong is
expensive: a wrong directory either hides the host's VA-API (the bug that made
VA-API unavailable on RDNA4) or mixes host and bundled modules. These fixtures
run the real scripts against fake payload/host trees and assert the decision
they publish through LD_LIBRARY_PATH.

No library is executed here; the loader ordering itself is covered by the
container smoke tests (packaging/smoke_appimage.py --expect-host-libva).
"""

import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).parents[1]
SCRIPTS = (ROOT / "packaging/AppRun", ROOT / "packaging/DiagnosticAppRun")
LIBS = ("libva.so.2", "libva-drm.so.2", "libva-x11.so.2")
ELF64 = b"\x7fELF\x02\x01\x01" + b"\0" * 9
ELF32 = b"\x7fELF\x01\x01\x01" + b"\0" * 9


class LauncherCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="trophy-apprun-", dir="/tmp/opencode"
                                        if Path("/tmp/opencode").is_dir() else None))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.appdir = self.tmp / "appdir"
        (self.appdir / "usr/bin").mkdir(parents=True)
        # Stand-in payload binary: it only reports the environment the launcher
        # built, so the fixture never needs a real Trophy build.
        probe = self.appdir / "usr/bin/trophy"
        probe.write_text('#!/bin/sh\nprintf "%s\\n" "${LD_LIBRARY_PATH:-}"\n')
        probe.chmod(0o755)
        self.payload_lib = self.appdir / "usr/lib"

    def host_dir(self, name, *, complete=True, cls=ELF64):
        directory = self.tmp / name
        directory.mkdir()
        for lib in LIBS:
            if not complete and lib == "libva-x11.so.2":
                continue
            (directory / lib).write_bytes(cls)
        return directory

    def run_apprun(self, script=SCRIPTS[0], *, host_dirs=None, prefer_bundled=False,
                   xdg="auto", tmpdir="auto", extra_env=None):
        env = {
            "PATH": os.environ["PATH"],
            "HOME": str(self.tmp),
            "APPDIR": str(self.appdir),
        }
        env.pop("LD_LIBRARY_PATH", None)
        if xdg == "auto":
            env["XDG_RUNTIME_DIR"] = str(self.runtime_dir())
        elif xdg is not None:
            env["XDG_RUNTIME_DIR"] = str(xdg)
        if tmpdir == "auto":
            env["TMPDIR"] = str(self.tmp / "tmp")
            Path(env["TMPDIR"]).mkdir(exist_ok=True)
        elif tmpdir is not None:
            env["TMPDIR"] = str(tmpdir)
        if host_dirs is not None:
            env["TROPHY_LIBVA_DIRS"] = ":".join(str(d) for d in host_dirs)
        if prefer_bundled:
            env["TROPHY_PREFER_BUNDLED_LIBVA"] = "1"
        env.update(extra_env or {})
        result = subprocess.run(["/bin/sh", str(script)], env=env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def runtime_dir(self):
        directory = self.tmp / "runtime"
        directory.mkdir(exist_ok=True)
        return directory

    def override(self):
        return self.runtime_dir() / "trophy-va"

    def test_complete_host_set_is_preferred(self):
        host = self.host_dir("host")
        value = self.run_apprun(host_dirs=[host])
        self.assertEqual(value.split(":"), [str(self.override()), str(self.payload_lib)])
        self.assertEqual(stat.S_IMODE(self.override().stat().st_mode), 0o700)
        for lib in LIBS:
            link = self.override() / lib
            self.assertTrue(link.is_symlink())
            self.assertEqual(link.resolve(), (host / lib).resolve())

    def test_missing_module_falls_back_to_bundled(self):
        # All-or-nothing: a host with only part of the set must not be mixed
        # with the bundled modules, because FFmpeg links all three.
        host = self.host_dir("host", complete=False)
        value = self.run_apprun(host_dirs=[host])
        self.assertEqual(value, str(self.payload_lib))
        self.assertFalse((self.override() / "libva.so.2").exists())

    def test_32bit_host_library_is_rejected(self):
        # ldconfig also lists multilib copies; a 32-bit ELF must never enter a
        # 64-bit process.
        host = self.host_dir("host32", cls=ELF32)
        value = self.run_apprun(host_dirs=[host])
        self.assertEqual(value, str(self.payload_lib))

    def test_second_complete_directory_is_used(self):
        bad = self.host_dir("host-bad", complete=False)
        good = self.host_dir("host-good")
        value = self.run_apprun(host_dirs=[bad, good])
        self.assertTrue(value.startswith(str(self.override()) + ":"))
        self.assertEqual((self.override() / "libva.so.2").resolve(),
                         (good / "libva.so.2").resolve())

    def test_prefer_bundled_escape_hatch(self):
        host = self.host_dir("host")
        value = self.run_apprun(host_dirs=[host], prefer_bundled=True)
        self.assertEqual(value, str(self.payload_lib))

    def test_stale_symlink_is_replaced(self):
        host = self.host_dir("host")
        self.override().mkdir(parents=True)
        stale = self.override() / "libva.so.2"
        stale.symlink_to("/nonexistent/libva.so.2")
        value = self.run_apprun(host_dirs=[host])
        self.assertTrue(value.startswith(str(self.override()) + ":"))
        self.assertEqual(stale.resolve(), (host / "libva.so.2").resolve())

    def test_tmpdir_fallback_when_no_runtime_dir(self):
        host = self.host_dir("host")
        tmpdir = self.tmp / "fallback-tmp"
        tmpdir.mkdir()
        value = self.run_apprun(host_dirs=[host], xdg=None, tmpdir=tmpdir)
        expected = tmpdir / f"trophy-va-{os.getuid()}"
        self.assertEqual(value.split(":"), [str(expected), str(self.payload_lib)])
        self.assertEqual(stat.S_IMODE(expected.stat().st_mode), 0o700)

    def test_symlinked_override_directory_is_refused(self):
        # A pre-created symlink must not be adopted: the launcher would write
        # into a directory it does not control.
        host = self.host_dir("host")
        elsewhere = self.tmp / "elsewhere"
        elsewhere.mkdir()
        self.override().symlink_to(elsewhere)
        value = self.run_apprun(host_dirs=[host])
        self.assertEqual(value, str(self.payload_lib))
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_existing_directory_is_tightened(self):
        host = self.host_dir("host")
        self.override().mkdir(parents=True)
        self.override().chmod(0o755)
        self.run_apprun(host_dirs=[host])
        self.assertEqual(stat.S_IMODE(self.override().stat().st_mode), 0o700)

    def test_caller_library_path_is_preserved_after_bundle(self):
        host = self.host_dir("host")
        value = self.run_apprun(host_dirs=[host],
                                extra_env={"LD_LIBRARY_PATH": "/caller/lib"})
        self.assertEqual(value.split(":"),
                         [str(self.override()), str(self.payload_lib), "/caller/lib"])

    def test_no_host_libraries_keeps_bundled(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        value = self.run_apprun(host_dirs=[empty])
        self.assertEqual(value, str(self.payload_lib))

    def test_diagnostic_launcher_matches_release(self):
        # The diagnostic AppRun duplicates the selection logic; identical
        # outcomes here are what keeps the two files from drifting apart.
        host = self.host_dir("host")
        release = self.run_apprun(SCRIPTS[0], host_dirs=[host])
        diagnostic = self.run_apprun(SCRIPTS[1], host_dirs=[host])
        self.assertEqual(release, diagnostic)
        incomplete = self.host_dir("host-incomplete", complete=False)
        self.assertEqual(
            self.run_apprun(SCRIPTS[0], host_dirs=[incomplete]),
            self.run_apprun(SCRIPTS[1], host_dirs=[incomplete]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
