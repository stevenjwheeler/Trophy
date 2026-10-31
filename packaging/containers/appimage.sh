#!/bin/bash
# Run inside appimage.Dockerfile, with read-only /source and owned writable /work.
set -euo pipefail
: "${VERSION:?set an explicit diagnostic build label}"
test "$(. /etc/os-release; echo "$ID:$VERSION_ID")" = 'ubuntu:24.04'
export HOME=/work/home LANG=C.UTF-8 APPIMAGE_EXTRACT_AND_RUN=1
export CCACHE_DIR=/work/ccache CCACHE_BASEDIR=/source CCACHE_MAXSIZE=1G
mkdir -p "$HOME" /work/tools
cmake -S /source -B /work/build -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON -DAPP_VERSION="$VERSION" -DCMAKE_INSTALL_PREFIX=/usr \
    -DQT_QML_GENERATE_QMLLS_INI=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
read -ra tests <<< "$(PYTHONPATH=/source/packaging python3 -c 'from build_friend_bundle import TESTS; print(" ".join(TESTS))')"
cmake --build /work/build --parallel 2 --target TrophyApp trophy_layer "${tests[@]}"
DESTDIR=/work/stage cmake --install /work/build
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    wget --tries=3 --waitretry=5 -q -O "/work/tools/$tool-x86_64.AppImage" \
        "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage"
    chmod +x "/work/tools/$tool-x86_64.AppImage"
done
export QMAKE="$QT_ROOT_DIR/bin/qmake" QML_SOURCES_PATHS=/work/build/qml
export LD_LIBRARY_PATH="$QT_ROOT_DIR/lib"
export EXTRA_QT_MODULES='quickcontrols2;multimedia;waylandcompositor'
plugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
wayland=("$plugins"/platforms/libqwayland*.so)
test -f "${wayland[0]}"
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
for plugin in "${wayland[@]}"; do
    EXTRA_PLATFORM_PLUGINS+=";$(basename "$plugin")"
done
provenance=()
if [[ -f /source/source-provenance.json ]]; then
    provenance=(--source-provenance /source/source-provenance.json)
fi
python3 /source/packaging/build_friend_bundle.py "${provenance[@]}" --appdir /work/stage \
    --build /work/build --linuxdeploy /work/tools/linuxdeploy-x86_64.AppImage \
    --qt-plugin --version "$VERSION" --output /work/friend-kit
python3 /source/packaging/smoke_appimage.py /work/friend-kit/Trophy.AppImage \
    --log /work/friend-startup.log
