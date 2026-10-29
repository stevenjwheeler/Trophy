# Developer/CI tooling only: none of this filesystem is sent to users.
FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build ccache pkg-config python3 python3-venv \
    ca-certificates wget git binutils file zstd xz-utils p7zip-full \
    libgl1-mesa-dev libx11-dev libxext-dev libxkbcommon-dev libwayland-dev \
    libxcb-shm0-dev libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxkbcommon-x11-0 libvulkan-dev \
    libpipewire-0.3-dev libspa-0.2-dev pipewire pipewire-bin wireplumber \
    libdrm-dev libgbm-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libva-dev libva-drm2 libgtest-dev \
    desktop-file-utils appstream ffmpeg dbus-daemon nodejs \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.3.0 \
    && /opt/aqt/bin/aqt install-qt linux desktop 6.12.0 linux_gcc_64 \
       --outputdir /opt/Qt --modules qtmultimedia qtwaylandcompositor
ENV PATH="/opt/Qt/6.12.0/gcc_64/bin:${PATH}" \
    CMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64 \
    QT_ROOT_DIR=/opt/Qt/6.12.0/gcc_64
