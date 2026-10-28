#!/bin/bash
# Distro-native package build; source is read-only, outputs stay in /work,/dist.
set -euo pipefail
profile="${1:?expected deb or rpm}"
: "${VERSION:?set the intended artifact version}"
export HOME=/work/home LANG=C.UTF-8
mkdir -p "$HOME" /dist
case "$profile" in
  deb) generators='TXZ;DEB'; extension=deb ;;
  rpm) generators=RPM; extension=rpm ;;
  *) echo 'Expected deb or rpm' >&2; exit 2 ;;
esac
cache_options=()
if command -v ccache >/dev/null; then
    export CCACHE_DIR=/work/ccache CCACHE_BASEDIR=/source CCACHE_MAXSIZE=1G
    cache_options=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
cmake -S /source -B /work/ci-build -GNinja "${cache_options[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAPP_VERSION="$VERSION" \
    -DCMAKE_INSTALL_PREFIX=/usr -DQT_QML_GENERATE_QMLLS_INI=OFF
cmake --build /work/ci-build --parallel 2
if command -v ccache >/dev/null; then ccache --show-stats; fi
cpack --config /work/ci-build/CPackConfig.cmake -G "$generators" -B /work/ci-packages
# A test-only later version exercises the real package-manager upgrade path;
# this package is never copied to the release artifact directory.
cpack --config /work/ci-build/CPackConfig.cmake -G "${extension^^}" \
    -D "CPACK_PACKAGE_VERSION=${VERSION#v}.verification.2" -B /work/ci-upgrade
mkdir -p /work/ci-payload
if [[ "$profile" == deb ]]; then
    packages=(/work/ci-packages/*.deb)
    dpkg-deb --extract "${packages[0]}" /work/ci-payload
    install -m644 "${packages[0]}" "/dist/Trophy-${VERSION}-amd64.deb"
    archives=(/work/ci-packages/*.tar.xz)
    python3 /source/packaging/smoke_native.py "${archives[0]}" --log /work/txz-startup.log
    install -m644 "${archives[0]}" "/dist/Trophy-${VERSION}.tar.xz"
else
    packages=(/work/ci-packages/*.rpm)
    # Avoid rpm2cpio SIGPIPE on archive padding when cpio finishes early.
    rpm2cpio "${packages[0]}" > /work/ci-payload.cpio
    (cd /work/ci-payload && cpio -idm --no-absolute-filenames < /work/ci-payload.cpio)
    install -m644 "${packages[0]}" "/dist/Trophy-${VERSION}-x86_64.rpm"
fi
python3 /source/packaging/smoke_native.py /work/ci-payload --log /work/native-startup.log
