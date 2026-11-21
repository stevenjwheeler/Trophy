"""ELF version-needs parser and pinned SDK/baseline fixtures."""

import importlib.util
import os
from pathlib import Path
import subprocess
import re
import shutil
import tempfile
import textwrap
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("abi", Path(__file__).resolve().parents[1] / "packaging/check_appimage_abi.py")
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)


class AppImageAbiTest(unittest.TestCase):
    def run_sdk_dependency_checks(self, **overrides):
        root = Path(__file__).resolve().parents[1]
        action = (root / ".github/actions/setup-sdk/action.yml").read_text()
        block = re.search(r"(?ms)^      run: \|\n(.*?)(?=^    - name:)", action)
        self.assertIsNotNone(block)
        script = textwrap.dedent(block.group(1))
        self.assertNotIn("vulkan.h", script)
        self.assertNotIn("libvulkan-dev", script)
        self.assertNotIn("multilib", script)
        self.assertIn(". /etc/os-release", script)
        script = script.replace(". /etc/os-release", '. "$SDK_TEST_OS_RELEASE"')
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            tools = directory / "bin"
            tools.mkdir()
            os_release = directory / "os-release"
            os_release.write_text("ID=ubuntu\nVERSION_ID=24.04\n")
            log = directory / "apt.log"
            stubs = {
                "sudo": 'printf "%s\\n" "$*" >> "$SDK_TEST_APT_LOG"\nexit 0\n',
                "pkg-config": (
                    'for package in "$@"; do\n'
                    '  [ "$package" != "${SDK_TEST_MISSING_PKG:-}" ] || exit 1\n'
                    'done\nexit "${SDK_TEST_PKG_STATUS:-0}"\n'
                ),
                "ffmpeg": 'printf "%s\\n" "${SDK_TEST_ENCODER:-V libx264}"\n',
            }
            for tool in ("ffprobe", "pipewire", "wireplumber", "pw-cli",
                         "pw-dump", "dbus-run-session", "zstd"):
                stubs[tool] = "exit 0\n"
            for name, body in stubs.items():
                path = tools / name
                path.write_text("#!/bin/sh\n" + body)
                path.chmod(0o755)
            environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.defpath,
                               RUNNER_TEMP=str(directory),
                               SDK_TEST_OS_RELEASE=str(os_release),
                               SDK_TEST_APT_LOG=str(log), **overrides)
            result = subprocess.run(["bash", "-c", script], env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertIn("apt-get install", log.read_text())
            return result

    def test_sdk_dependency_setup_does_not_require_removed_reporters(self):
        result = self.run_sdk_dependency_checks()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sdk_dependency_checks_still_reject_missing_pipewire(self):
        self.assertNotEqual(
            self.run_sdk_dependency_checks(SDK_TEST_PKG_STATUS="1").returncode, 0)

    def test_sdk_dependency_checks_still_require_software_h264(self):
        self.assertNotEqual(
            self.run_sdk_dependency_checks(SDK_TEST_ENCODER="No encoder").returncode, 0)

    def test_sdk_requires_launcher_metadata_libraries(self):
        for package in ("yaml-0.1", "sqlite3"):
            with self.subTest(package=package):
                self.assertNotEqual(self.run_sdk_dependency_checks(
                    SDK_TEST_MISSING_PKG=package).returncode, 0)

    def test_package_build_environments_install_launcher_metadata_headers(self):
        root = Path(__file__).resolve().parents[1]
        action = (root / ".github/actions/setup-sdk/action.yml").read_text()
        self.assertIn("libyaml-dev libsqlite3-dev", action)
        for filename in ("ubuntu.Dockerfile", "appimage.Dockerfile"):
            dockerfile = (root / "packaging/containers" / filename).read_text()
            self.assertIn("libyaml-dev libsqlite3-dev", dockerfile)
        fedora = (root / "packaging/containers/fedora.Dockerfile").read_text()
        self.assertIn("libyaml-devel sqlite-devel", fedora)

    def test_all_sdk_handoff_jobs_share_the_pinned_runner(self):
        root = Path(__file__).resolve().parents[1]
        workflow = (root / ".github/workflows/ci.yml").read_text()
        for job in ("build", "test", "package-appimage"):
            block = re.search(r"(?ms)^  " + job + r":\n(.*?)(?=^  [\w-]+:|\Z)", workflow).group(1)
            self.assertIn("runs-on: ubuntu-24.04", block)
        self.assertIn("packaging/check_appimage_abi.py", workflow)
        self.assertIn("trophy-appimage-abi.json", workflow)
        action = (root / ".github/actions/setup-sdk/action.yml").read_text()
        self.assertIn('"ubuntu:24.04"', action)

    def test_ignores_exported_versions_but_reads_all_needs(self):
        output = """Version definition section '.gnu.version_d' contains 2 entries:
  Name: GLIBC_2.44
Version needs section '.gnu.version_r' contains 3 entries:
  Name: GLIBC_2.39 Flags: none
  Name: GLIBCXX_3.4.32 Flags: none
  Name: CXXABI_1.3.14 Flags: none
  Name: GLIBC_ABI_DT_RELR Flags: none
"""
        versions = abi.required_versions(output)
        self.assertNotIn("GLIBC_2.44", versions)
        self.assertEqual(len(versions), 4)
        self.assertEqual(abi.violations(versions), [])

    def test_numeric_order_not_lexical_order(self):
        self.assertEqual(abi.violations({"GLIBC_2.9", "GLIBCXX_3.4.9", "CXXABI_1.3.9"}), [])

    def test_newer_glibc_and_cpp_are_rejected_independently(self):
        for version in ("GLIBC_2.44", "GLIBCXX_3.4.33", "CXXABI_1.3.15", "GLIBC_PRIVATE", "GLIBC_ABI_UNKNOWN"):
            with self.subTest(version=version):
                self.assertEqual(len(abi.violations({version})), 1)

    def test_unversioned_and_non_system_names_are_ignored(self):
        self.assertEqual(abi.required_versions("Version needs section 'x':\n Name: Qt_6.12\n"), set())

    def test_scans_nested_plugins_not_just_main_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "usr/plugins").mkdir(parents=True)
            app = root / "usr/app"
            plugin = root / "usr/plugins/plugin.so"
            app.write_bytes(b"\x7fELFfixture")
            plugin.write_bytes(b"\x7fELFfixture")
            def read(command, **kwargs):
                version = "2.44" if command[-1].endswith("plugin.so") else "2.39"
                return "Version needs section 'x':\n Name: GLIBC_" + version
            with mock.patch.object(abi.subprocess, "check_output", side_effect=read):
                report = abi.scan(root)
            self.assertFalse(report["passed"])
            self.assertEqual(report["maximum_required"]["GLIBC"], "2.44")
            self.assertEqual(report["offenders"][0]["file"], "usr/plugins/plugin.so")

    def test_empty_or_failed_inspection_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(RuntimeError):
                abi.scan(root)
            (root / "broken").write_bytes(b"\x7fELFbroken")
            with mock.patch.object(abi.subprocess, "check_output", side_effect=subprocess.CalledProcessError(1, [])):
                with self.assertRaises(subprocess.CalledProcessError):
                    abi.scan(root)

    def test_real_linked_elf_future_version_is_rejected(self):
        if not all(shutil.which(tool) for tool in ("cc", "readelf")):
            self.skipTest("Native ELF fixture requires cc and readelf")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "future.c").write_text("int future_api(void) { return 1; }\n")
            (root / "future.map").write_text("GLIBC_2.44 { global: future_api; local: *; };\n")
            (root / "main.c").write_text("extern int future_api(void); int main(void) { return future_api(); }\n")
            subprocess.run(["cc", "-shared", "-fPIC", str(root / "future.c"),
                            "-Wl,--version-script=" + str(root / "future.map"),
                            "-o", str(root / "libfuture.so")], check=True, capture_output=True)
            subprocess.run(["cc", str(root / "main.c"), "-L" + str(root), "-lfuture",
                            "-o", str(root / "app")], check=True, capture_output=True)
            report = abi.scan(root)
            self.assertFalse(report["passed"])
            self.assertIn("app", [item["file"] for item in report["offenders"]])
            self.assertNotIn("libfuture.so", [item["file"] for item in report["offenders"]])


if __name__ == "__main__":
    unittest.main()
