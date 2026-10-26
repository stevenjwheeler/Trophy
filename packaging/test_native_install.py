"""Install/reinstall/upgrade/remove Trophy ONLY inside an explicitly disposable container.

Never mount host system/config/runtime directories into that container. This is
a headless package-manager/dependency test, not capture or desktop validation.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from smoke_native import verify


def require_container():
    if os.environ.get("TROPHY_PACKAGE_TEST_CONTAINER") != "1" or not (
            Path("/.dockerenv").is_file() or Path("/run/.containerenv").is_file()):
        raise RuntimeError("Refusing package-manager operations outside an explicitly disposable container")
    if os.geteuid() != 0:
        raise RuntimeError("Package-manager checks require container root, never host sudo")


def test_package(package, upgrade, logs):
    require_container()  # Must precede every package-manager operation.
    logs.mkdir(parents=True, exist_ok=True)
    manager = "deb" if package.suffix == ".deb" else "rpm" if package.suffix == ".rpm" else ""
    if not manager or (upgrade and upgrade.suffix != package.suffix):
        raise RuntimeError("Expected a .deb or .rpm and an optional matching-format upgrade")

    def output(args):
        return subprocess.check_output(args, text=True, timeout=60).strip()

    def metadata(path):
        name = output(["dpkg-deb", "--field", str(path), "Package"]) if manager == "deb" else output(
            ["rpm", "-qp", "--qf", "%{NAME}", str(path)])
        if name != "trophy":
            raise RuntimeError(f"Refusing non-Trophy package {name}")
        return output(["dpkg-deb", "--field", str(path), "Version"]) if manager == "deb" else output(
            ["rpm", "-qp", "--qf", "%{VERSION}-%{RELEASE}", str(path)])

    versions = [metadata(package)] + ([metadata(upgrade)] if upgrade else [])
    if upgrade and versions[0] == versions[1]:
        raise RuntimeError("Upgrade must have a different package version")
    installed_query = ["dpkg-query", "-W", "-f=${Version}", "trophy"] if manager == "deb" else [
        "rpm", "-q", "--qf", "%{VERSION}-%{RELEASE}", "trophy"]
    if subprocess.run(installed_query, capture_output=True, timeout=60).returncode == 0:
        raise RuntimeError("Refusing a container where Trophy is already installed")

    def run(name, args):
        with (logs / f"{name}.log").open("w") as log:
            subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)

    if manager == "deb":
        run("apt-update", ["apt-get", "update"])
        install = ["apt-get", "install", "-y", "--no-install-recommends"]
        reinstall = install + ["--reinstall"]
        remove = ["apt-get", "remove", "-y", "trophy"]
        owned_query = ["dpkg-query", "-L", "trophy"]
    else:
        install = ["dnf", "install", "-y", "--setopt=install_weak_deps=False"]
        reinstall = ["dnf", "reinstall", "-y", "--setopt=install_weak_deps=False"]
        remove = ["dnf", "remove", "-y", "trophy"]
        owned_query = ["rpm", "-ql", "trophy"]
    run("install", install + [str(package)])
    if output(installed_query) != versions[0]:
        raise RuntimeError("Installed version does not match input package")
    verify(Path("/usr"), logs / "installed-startup.log")
    run("reinstall", reinstall + [str(package)])
    verify(Path("/usr"), logs / "reinstalled-startup.log")
    if upgrade:
        run("upgrade", install + [str(upgrade)])
        if output(installed_query) != versions[1]:
            raise RuntimeError("Package-manager upgrade did not adopt the new version")
        verify(Path("/usr"), logs / "upgraded-startup.log")
    owned = [Path(name) for name in output(owned_query).splitlines()]
    files = [path for path in owned if path.is_file() or path.is_symlink()]
    run("remove", remove)
    remaining = [str(path) for path in files if path.exists() or path.is_symlink()]
    if remaining:
        raise RuntimeError(f"Package-owned files remain after removal: {remaining}")
    if manager == "deb":
        audit = output(["dpkg", "--audit"])
        if audit:
            raise RuntimeError(f"dpkg audit is not clean: {audit}")
    report = {"versions": versions, "os_release": Path("/etc/os-release").read_text(),
              "sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in [package] + ([upgrade] if upgrade else [])},
              "scope": "Disposable-container install/reinstall/upgrade/removal and headless startup; no live capture"}
    (logs / "report.json").write_text(json.dumps(report, indent=2))
    print(f"{manager} package-manager checks passed: {versions}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--upgrade", type=Path)
    parser.add_argument("--logs", type=Path, required=True)
    args = parser.parse_args()
    try:
        test_package(args.package.absolute(), args.upgrade.absolute() if args.upgrade else None, args.logs)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Container package test failed: {error}; logs: {args.logs}\n")
