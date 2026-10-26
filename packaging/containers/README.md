# Distro-native builds and clean package-manager checks

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
