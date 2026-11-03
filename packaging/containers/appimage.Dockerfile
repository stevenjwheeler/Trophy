# Developer/CI tooling only: none of this filesystem is sent to users.
# Build with the packaging/ directory as context so the FFmpeg script can be
# copied in (see packaging/containers/README.md):
#   docker build -f packaging/containers/appimage.Dockerfile packaging/
FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build ccache pkg-config python3 python3-venv \
    ca-certificates wget git binutils file zstd xz-utils p7zip-full \
    libgl1-mesa-dev libx11-dev libxext-dev libxkbcommon-dev libwayland-dev \
    libxcb-shm0-dev libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxkbcommon-x11-0 libvulkan-dev \
    libpipewire-0.3-dev libspa-0.2-dev pipewire pipewire-bin wireplumber \
    libdrm-dev libgbm-dev libva-dev libva-drm2 libgtest-dev \
    nasm libx264-dev libopus-dev zlib1g-dev \
    desktop-file-utils appstream ffmpeg dbus-daemon nodejs \
    && rm -rf /var/lib/apt/lists/*

# One slim FFmpeg 9 for both libTrophy and Qt Multimedia. This replaces the
# Ubuntu 24.04 FFmpeg 6 the app used to link plus the Qt SDK's LGPL FFmpeg 9.
COPY ffmpeg/build-ffmpeg.sh /usr/local/bin/build-ffmpeg
COPY ffmpeg/check-components.c /tmp/check-components.c
RUN chmod +x /usr/local/bin/build-ffmpeg \
    && PREFIX=/opt/trophy-ffmpeg JOBS="$(nproc)" /usr/local/bin/build-ffmpeg \
    && echo /opt/trophy-ffmpeg/lib > /etc/ld.so.conf.d/trophy-ffmpeg.conf \
    && ldconfig \
    && PKG_CONFIG_PATH=/opt/trophy-ffmpeg/lib/pkgconfig \
       sh -c 'cc /tmp/check-components.c $(pkg-config --cflags --libs libavcodec libavformat libavutil) -o /tmp/check-components' \
    && /tmp/check-components \
    && rm -f /tmp/check-components /tmp/check-components.c

RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.3.0 \
    && /opt/aqt/bin/aqt install-qt linux desktop 6.12.0 linux_gcc_64 \
       --outputdir /opt/Qt --modules qtmultimedia qtwaylandcompositor

# Drop the Qt SDK's duplicate FFmpeg (same sonames, but LGPL and without the
# software H.264 encoder). Qt's multimedia plugin then loads ours through the
# ld.so cache, which is what makes a single bundled generation possible.
RUN rm -f /opt/Qt/6.12.0/gcc_64/lib/libav*.so* /opt/Qt/6.12.0/gcc_64/lib/libsw*.so* \
    && ldconfig

# /opt/trophy-ffmpeg/bin first: the test fixtures decode through the ffmpeg CLI
# and through the libraries, so both must be the same FFmpeg generation.
ENV PATH="/opt/trophy-ffmpeg/bin:/opt/Qt/6.12.0/gcc_64/bin:${PATH}" \
    CMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64 \
    QT_ROOT_DIR=/opt/Qt/6.12.0/gcc_64 \
    PKG_CONFIG_PATH=/opt/trophy-ffmpeg/lib/pkgconfig
