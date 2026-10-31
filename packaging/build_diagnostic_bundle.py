"""Add source-free unit/GPU tests to a staged AppDir and make one diagnostic AppImage."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from check_appimage_abi import check as check_abi


# Deliberately excludes tests that contact the frozen detection socket, require
# source-tree paths, change compositor state or need private native services.
TESTS = (
    "test_EncoderTest", "test_DmaBufCudaImport", "test_VaapiVideoProcessor",
    "test_EglDisplayLease", "test_Muxer", "test_EncoderFactory", "test_AudioEncoder",
    "test_AudioDriftCorrector", "test_FramePipeline", "test_PipeWireVideoFormat",
    "test_PipeWireBufferLease", "test_PipeWireSyncWait", "test_AudioMixer",
)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def build_id(path):
    import re
    output = subprocess.check_output(["readelf", "-n", str(path)], text=True)
    match = re.search(r"Build ID:\s*(\w+)", output)
    if not match:
        raise RuntimeError("ELF build ID missing: " + str(path))
    return match.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True, help="Existing staged AppDir from this build")
    parser.add_argument("--build", type=Path, required=True, help="Matching completed BUILD_TESTING=ON build")
    parser.add_argument("--linuxdeploy", type=Path, required=True, help="Trusted linuxdeploy AppImage/tool")
    parser.add_argument("--output", type=Path, required=True, help="New directory outside the source tree")
    parser.add_argument("--version", required=True)
    parser.add_argument("--qt-plugin", action="store_true", help="Run linuxdeploy's Qt plugin if the input AppDir is not yet deployed")
    parser.add_argument("--skip-abi-check", action="store_true", help="Skip the Ubuntu 24.04 ABI baseline gate (local/non-portable builds only)")
    parser.add_argument("--source-provenance", type=Path,
                        help="Trusted snapshot metadata when building without a .git directory")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output == source or source in output.parents or output.exists():
        parser.error("Output must be a new directory outside the source tree")
    appdir, build = args.appdir.resolve(), args.build.resolve()
    library = next(appdir.glob("usr/lib/**/libTrophy.so"), None)
    if library is None or build_id(library) != build_id(build / "libTrophy.so"):
        parser.error("AppDir and build libTrophy build IDs must match")
    if build_id(appdir / "usr/bin/trophy") != build_id(build / "trophy"):
        parser.error("AppDir and build trophy build IDs must match")
    for name in TESTS:
        if not (build / "tests" / name).is_file():
            parser.error("Required unit/GPU binary missing: " + name)
    if args.source_provenance:
        provenance = json.loads(args.source_provenance.read_text())
        if not isinstance(provenance.get("source_dirty"), bool) or not provenance.get("source_sha256"):
            parser.error("Snapshot provenance must include dirty state and source hashes")
        for name, expected in provenance["source_sha256"].items():
            path = (source / name).resolve()
            if not path.is_relative_to(source) or not path.is_file() or digest(path) != expected:
                parser.error("Source snapshot mismatch: " + name)
    else:
        provenance = {"source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
                      "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=source))}
    output.mkdir(parents=True, mode=0o700)
    stage = output / "AppDir"
    shutil.copytree(appdir, stage, symlinks=True)
    command = [str(args.linuxdeploy.resolve()), "--appdir", str(stage)]
    for name in TESTS:
        target = stage / "usr/bin" / name
        shutil.copyfile(build / "tests" / name, target)
        target.chmod(0o755)
        command += ["--executable", str(target)]
    command += ["--executable", str(stage / "usr/bin/trophy"),
                "--desktop-file", str(source / "packaging/trophy.desktop"),
                "--icon-file", str(source / "resources/icons/trophy.svg")]
    if args.qt_plugin:
        command += ["--plugin", "qt"]
    env = os.environ.copy()
    env.update(APPIMAGE_EXTRACT_AND_RUN="1", LINUXDEPLOY_OUTPUT_VERSION=args.version)
    subprocess.run(command, env=env, cwd=output, check=True)
    # Driver/interface libraries must come from the host: bundling them pins an
    # older version than the host's driver, which breaks the corresponding
    # feature (VA-API, Vulkan, VDPAU, OpenCL, oneVPL) on newer systems. Treat
    # them as host dependencies, like PipeWire/portal/drivers.
    # Use precise loader sonames (libfoo.so*) so Qt plugins such as
    # libvulkan-server.so are not mistaken for the host loader.
    for pattern in ("libva.so*", "libva-drm.so*", "libva-x11.so*",
                    "libvdpau.so*", "libvulkan.so*", "libOpenCL.so*",
                    "libvpl.so*"):
        for path in stage.rglob(pattern):
            path.unlink()
    vendor_libraries = [path for pattern in ("libcuda.so*", "libnvidia-*.so*",
                                            "*_drv_video.so", "*_dri.so",
                                            "libva.so*", "libva-drm.so*",
                                            "libva-x11.so*", "libvdpau.so*",
                                            "libvulkan.so*", "libOpenCL.so*",
                                            "libvpl.so*")
                        for path in stage.rglob(pattern)]
    if vendor_libraries:
        raise RuntimeError("Refusing a diagnostic AppImage with bundled vendor GPU drivers: " + str(vendor_libraries))
    abi = check_abi(stage)
    (output / "abi-report.json").write_text(json.dumps(abi, indent=2) + "\n")
    if not args.skip_abi_check and not abi["passed"]:
        raise RuntimeError("AppDir exceeds Ubuntu 24.04 ABI baseline; see abi-report.json. Rebuild on the baseline, not the host, or pass --skip-abi-check for a non-portable local build.")
    metadata = {"schema": 2, "version": args.version, "tests": list(TESTS),
                "scope": "Focused source-free unit and real GPU tests, not the full CI suite",
                **provenance,
                "build_ids": {name: build_id(build / "tests" / name) for name in TESTS},
                "app_build_id": build_id(build / "trophy"),
                "library_build_id": build_id(build / "libTrophy.so"),
                "minimum_glibc": abi["maximum_required"]["GLIBC"],
                "abi_baseline": abi["limits"]}
    location = stage / "usr/share/trophy/diagnostic-tests.json"
    location.parent.mkdir(parents=True, exist_ok=True)
    location.write_text(json.dumps(metadata, indent=2) + "\n")
    launcher = stage / "AppRun"
    if launcher.is_symlink():
        launcher.unlink()
    shutil.copyfile(source / "packaging/DiagnosticAppRun", launcher)
    launcher.chmod(0o755)
    subprocess.run([str(args.linuxdeploy.resolve()), "--appdir", str(stage), "--output", "appimage"],
                   env=env, cwd=output, check=True)
    images = list(output.glob("*.AppImage"))
    if len(images) != 1:
        raise RuntimeError("Expected exactly one output AppImage")
    images[0].rename(output / "Trophy.AppImage")
    (output / "Trophy.AppImage").chmod(0o755)
    abi = check_abi(output / "Trophy.AppImage")
    (output / "abi-report.json").write_text(json.dumps(abi, indent=2) + "\n")
    if not args.skip_abi_check and not abi["passed"]:
        raise RuntimeError("Finished diagnostic AppImage exceeds ABI baseline; see abi-report.json")
    shutil.copyfile(source / "packaging" / "collect_diagnostics.py", output / "collect_diagnostics.py")
    metadata["sha256"] = {name: digest(output / name) for name in (
        "Trophy.AppImage", "collect_diagnostics.py")}
    (output / "bundle.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Diagnostics kit:", output)
    print("Send Trophy.AppImage and collect_diagnostics.py.")
    print("Run: python3 collect_diagnostics.py --appimage ./Trophy.AppImage")
    print("AppDir is retained for developer inspection; no source/rootfs is distributed.")


if __name__ == "__main__":
    main()
