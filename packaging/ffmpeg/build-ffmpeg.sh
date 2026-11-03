#!/usr/bin/env bash
# Build the one FFmpeg generation the AppImage ships.
#
# Why we build FFmpeg ourselves
# -----------------------------
# The AppImage used to carry two incompatible generations:
#   * Ubuntu 24.04's FFmpeg 6 (libavcodec.so.60) for libTrophy, and
#   * the Qt 6.12 SDK's own LGPL FFmpeg 9 (libavcodec.so.63) for the Qt
#     Multimedia plugin.
# That duplicated ~40 MB and dragged in external codec/protocol libraries we
# never use (x265, SvtAv1, codec2, librsvg, libxml2 and a second ICU blob).
# Qt's copy cannot be reused for the app because it is LGPL and has no
# software H.264 encoder, i.e. no x264.
#
# This script builds a single shared FFmpeg 9 with the components Trophy and
# Qt Multimedia actually use. Both link the same libavcodec.so.63, so exactly
# one generation is bundled.
#
# Requirements (the caller installs these): build-essential, nasm, pkg-config,
# wget, xz-utils, git, zlib1g-dev, libva-dev, libx264-dev, libopus-dev.
#
# Usage (invoke through bash; checkouts on FUSE mounts may not keep the exec
# bit):
#   PREFIX=/opt/trophy-ffmpeg bash packaging/ffmpeg/build-ffmpeg.sh
#
# The result is a relocatable prefix (headers, versioned shared libraries,
# pkg-config files and the ffmpeg/ffprobe tools) that CI caches and the
# container images install. The tools are for the test fixtures only: they must
# be the same generation as the libraries, because tests decode a fixture both
# through the libraries and through the ffmpeg CLI and compare the results.
# Nothing from bin/ is bundled into the AppImage.

set -euo pipefail

FFMPEG_VERSION=9.0.2
# Official release tarball from ffmpeg.org (sha256 of ffmpeg-9.0.2.tar.xz).
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

# NVENC headers. This is the oldest tag FFmpeg 9 compiles against, and it
# matches the Ubuntu 24.04 ffnvcodec generation the previous bundle used, so
# the minimum usable NVIDIA driver does not move. Do not bump this without
# checking the nvenc.c driver-version check: compiling against newer headers
# raises the driver version FFmpeg demands at runtime.
NV_CODEC_HEADERS_TAG=n12.0.16.2

PREFIX=${PREFIX:-/opt/trophy-ffmpeg}
SRC=${SRC:-/tmp/trophy-ffmpeg-src}
JOBS=${JOBS:-$(nproc)}

log() { printf '[build-ffmpeg] %s\n' "$*"; }
die() { printf '[build-ffmpeg] ERROR: %s\n' "$*" >&2; exit 1; }

for tool in wget tar git make pkg-config cc; do
    command -v "$tool" >/dev/null || die "missing required tool: $tool"
done
pkg-config --exists libva || die "libva development files are required for VA-API"
pkg-config --exists x264 || die "libx264 development files are required for software H.264"
pkg-config --exists opus || die "libopus development files are required for Opus audio"

mkdir -p "$SRC" "$PREFIX"
cd "$SRC"

# ---------------------------------------------------------------------------
# 1. NVENC headers
# ---------------------------------------------------------------------------
if [ ! -f "$PREFIX/include/ffnvcodec/nvEncodeAPI.h" ]; then
    log "installing nv-codec-headers $NV_CODEC_HEADERS_TAG"
    rm -rf nv-codec-headers
    git clone --depth 1 --branch "$NV_CODEC_HEADERS_TAG" \
        https://github.com/FFmpeg/nv-codec-headers.git
    make -C nv-codec-headers install PREFIX="$PREFIX"
fi

# ---------------------------------------------------------------------------
# 2. FFmpeg
# ---------------------------------------------------------------------------
archive="ffmpeg-$FFMPEG_VERSION.tar.xz"
if [ ! -f "$archive" ]; then
    log "downloading FFmpeg $FFMPEG_VERSION"
    wget -q --tries=3 --waitretry=5 -O "$archive.part" \
        "https://ffmpeg.org/releases/$archive"
    mv "$archive.part" "$archive"
fi
echo "$FFMPEG_SHA256  $archive" | sha256sum -c --quiet -

srcdir="ffmpeg-$FFMPEG_VERSION"
if [ ! -d "$srcdir" ]; then
    tar -xf "$archive"
fi
cd "$srcdir"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

log "configuring FFmpeg $FFMPEG_VERSION"
# Notes on the flags:
#   --disable-autodetect       Only what we name explicitly. This is what drops
#                              x265/SvtAv1/codec2/librsvg/libxml2/ICU and the
#                              protocol libraries from the closure.
#   --enable-gpl --enable-libx264  Software H.264 for recording, DVR and the
#                              share-export conversion (Qt's copy lacks it).
#   --enable-libopus           The native opus encoder is still flagged
#                              experimental, so avcodec_find_encoder() would
#                              return an encoder the app cannot open.
#   --enable-vaapi/--enable-nvenc  Hardware paths, resolved at runtime.
#   --disable-network          Local files only; no protocols pulling TLS.
#   (avdevice/avfilter stay on) Only the ffmpeg CLI uses them, for the test
#                              fixtures (-f lavfi synthetic sources). The app
#                              and Qt link neither, so linuxdeploy does not
#                              ship them: the bundle stays slim.
#   --disable-doc              No API documentation.
./configure \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --disable-autodetect \
    --enable-gpl --enable-libx264 --enable-libopus \
    --enable-vaapi --enable-nvenc --enable-ffnvcodec \
    --enable-zlib \
    --disable-network \
    --disable-vulkan --disable-libdrm --disable-xlib --disable-sdl2 --disable-iconv \
    --disable-doc \
    --enable-pic

log "building with $JOBS jobs"
make -j"$JOBS"
make install

# Record what this prefix is so a stale cache is obvious in the logs.
cat > "$PREFIX/BUILD-INFO" <<EOF
ffmpeg=$FFMPEG_VERSION
nv-codec-headers=$NV_CODEC_HEADERS_TAG
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

log "installed to $PREFIX"
log "libraries:"
find "$PREFIX/lib" -maxdepth 1 -name 'lib*.so.*' -printf '  %f %s bytes\n' | sort
