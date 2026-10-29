# Clean desktop-library baseline: no Qt SDK, FFmpeg or development packages.
FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 dbus-daemon binutils libstdc++6 libgl1 libegl1 libopengl0 \
    libxkbcommon0 libxcb-cursor0 libxkbcommon-x11-0 fonts-dejavu-core \
    libfontconfig1 libfreetype6 libharfbuzz0b libfribidi0 \
    libpipewire-0.3-0 libpipewire-0.3-common \
    && rm -rf /var/lib/apt/lists/*
