# AppImage and distro-native builds

## AppImage baseline builder

The SDK build/test/AppImage CI jobs all run on `ubuntu-24.04`, not the moving
`ubuntu-latest` label. `appimage.Dockerfile` provides the same OS/Qt 6.12.0 baseline
for local diagnostic builds. Docker is **developer-only**: users receive a normal
AppImage and Python launcher, not this filesystem or a Docker prerequisite.

```sh
docker build -t trophy-appimage-ubuntu24 - < packaging/containers/appimage.Dockerfile
work=$(mktemp -d /tmp/opencode/trophy-appimage-XXXXXX)
docker run --rm --cpus 2 --memory 4g --cap-drop ALL \
    --security-opt no-new-privileges --user "$(id -u):$(id -g)" \
    --env VERSION=0.9.7-friend-test \
    --mount "type=bind,src=$PWD,dst=/source,readonly" \
    --mount "type=bind,src=$work,dst=/work" \
    trophy-appimage-ubuntu24 bash /source/packaging/containers/appimage.sh
```

The script builds the application/reporters and 13 focused source-free unit/GPU
test executables, stages only under `/work`, packages a diagnostic AppImage,
checks its ABI and smokes startup. It does not run the full CI suite or claim GPU
coverage inside this device-free builder. Run the resulting kit on real hardware
to validate native encode/decode/import/resize. No host home/runtime, services,
GPU devices or detection socket are mounted into the builder.

`appimage-runtime.Dockerfile` is the separate clean Ubuntu 24.04 startup baseline:
only Python/D-Bus and ordinary desktop graphics/font/PipeWire client libraries,
no Qt SDK, FFmpeg or development packages. PipeWire client libraries are an
intentional linuxdeploy host dependency; no host daemon is connected or started.
CI smokes the exact AppImage in this network-disabled,
read-only runtime before uploading it; its private `/tmp` keeps service/socket
tests away from the host. This catches dependencies accidentally supplied only
by the SDK builder. Real Wayland rendering, portal capture and GPU execution
still need the separate hardware test kit.

Use a local source checkout readable by Docker. For a FUSE checkout that Docker
cannot access, use an ordinary-file snapshot plus `source-provenance.json` with
`source_revision`, boolean `source_dirty`, and a `source_sha256` path/hash map.
The packager verifies those hashes rather than inventing a Git revision or
discarding local changes. The source snapshot is not included in the sent kit.

Both staged and finished AppImages must pass `check_appimage_abi.py`: required
GLIBC <= 2.39, GLIBCXX <= 3.4.32 and CXXABI <= 1.3.14, across all ELF libraries,
plugins and binaries. Preserve `abi-report.json` with build evidence. The base
digest/Qt version are pinned; apt updates and continuous linuxdeploy tools remain
mutable, so this is a compatibility baseline, not a bit-reproducible build claim.

## Distro-native package targets

The main Qt SDK build is for the AppImage. Native packages instead use the
distribution's own Qt/FFmpeg stack: Ubuntu 26.04 for DEB/TXZ and Fedora 44 for
RPM. An Ubuntu-built RPM was rejected by clean Fedora because Qt's private-ABI
symbol versions differ; bypassing dependency checks would not fix that ABI.
Likewise, ELF dependency scanning alone missed Debian's QML runtime plugins.
The explicit runtime dependencies in CMake and these clean-install checks
address those distinct problems.

Dockerfiles pin the official base-image digests; distribution repository
packages can still receive updates. These are not bit-for-bit reproducible
builds or claims of compatibility with older distributions. No host services,
GPU devices, detection sockets or configuration directories are mounted.

## CI parallelism and caches

CI runs for pull requests targeting `main` and pushes to `main`, not standalone
pushes to `dev`, other branches or tags. A release starts as a reviewed `VERSION`
change in a dev-to-main PR. PR builds use a PR-number/SHA version suffix; main
builds use the plain version from that file. Passing main CI can create the tag
and **draft** release, but publication is always manual. See the complete
[release procedure](../README.md#drafting-a-release), including existing-tag and
published-version safeguards. These containers do not create releases themselves.

CI builds the Qt SDK application/tests once, transfers that build in a
mode/symlink-preserving tar.zst, then runs tests without recompiling. After tests
pass, AppImage packaging and the Ubuntu/Fedora native package jobs run in
parallel. Each uploads its own packages immediately; a final job verifies all
four formats and uploads a small combined checksum manifest only after every
required check passes. Package bytes are not uploaded again as a duplicate ZIP.

Native jobs restore BuildKit layers from the GitHub Actions cache separately
for each distro's builder and clean runtime image. The runners/containers are
still disposable: no registry images are published and no mutable installed
runtime is reused for package lifecycle checks. A separate per-distro ccache
reuses compiler outputs, not package-manager state. A cold/missing/evicted cache
only makes a run slower; it does not bypass builds, tests or dependency checks.
Dockerfile/base-image changes invalidate the relevant layers. CI also changes
the dependency build argument weekly so repository updates are not hidden by a
permanent layer cache; base digests remain deliberately pinned. Clearing the
relevant Actions cache forces an earlier refresh when needed.

Run from a checkout on a filesystem readable by the Docker daemon (some FUSE
mounts are not; use a local source snapshot containing your intended changes).
Example Ubuntu build, using an owned temporary output directory:

```sh
work=$(mktemp -d)
mkdir -p "$work/dist" "$work/runtime"
docker build -t trophy-native-ubuntu - < packaging/containers/ubuntu.Dockerfile
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
    --user "$(id -u):$(id -g)" --env VERSION=0.9.7-verification \
    --mount "type=bind,src=$PWD,dst=/source,readonly" \
    --mount "type=bind,src=$work,dst=/work" \
    --mount "type=bind,src=$work/dist,dst=/dist" \
    trophy-native-ubuntu bash /source/packaging/containers/build.sh deb
```

`build.sh` checks the actual extracted payload (and relocated TXZ), copies only
the intended artifacts into `dist`, and separately generates a later test-only
upgrade package under `ci-upgrade`. Upgrade-test files are never release assets.
Use `fedora.Dockerfile` and profile `rpm` for a Fedora-native RPM; never substitute
an Ubuntu RPM just because both are x86_64.

Then test dependencies and package-manager lifecycle in a **new container**, not
the build image where development packages could hide missing dependencies:

```sh
docker build -t trophy-runtime-ubuntu - < packaging/containers/ubuntu-runtime.Dockerfile
package=$(find "$work/ci-packages" -maxdepth 1 -name '*.deb' -printf '%f\n')
upgrade=$(find "$work/ci-upgrade" -maxdepth 1 -name '*.deb' -printf '%f\n')
docker run --rm --env TROPHY_PACKAGE_TEST_CONTAINER=1 \
    --mount "type=bind,src=$PWD,dst=/source,readonly" \
    --mount "type=bind,src=$work,dst=/artifacts,readonly" \
    --mount "type=bind,src=$work/runtime,dst=/evidence" \
    trophy-runtime-ubuntu python3 /source/packaging/test_native_install.py \
    "/artifacts/ci-packages/$package" --upgrade "/artifacts/ci-upgrade/$upgrade" --logs /evidence
```

Fedora uses `fedora-runtime.Dockerfile` and `.rpm` inputs. Package installation
runs as root **only inside the disposable container**. The script requires both
the explicit marker and a container marker file, refuses pre-existing Trophy or
non-Trophy packages, and never invokes host sudo. These are accident-prevention
guards, not a security sandbox for untrusted packages. Only test trusted builds.

The harness checks install/reinstall/upgrade startup with private HOME/XDG/D-Bus,
matching installed versions, then verifies package-owned files disappear on
removal (and a clean dpkg audit on Debian). `report.json` records package hashes,
versions and target OS; startup and manager logs remain under `runtime`.
This is headless package lifecycle coverage, not live Wayland rendering,
portal capture, login integration, or validation of every future release asset.
