# Clean desktop-library baseline: no Qt SDK, FFmpeg or development packages.
#
# It deliberately includes the distribution Qt (6.4, older than the AppImage's
# bundled 6.12) because that is what a real desktop looks like: the launcher
# must keep using the bundled Qt and must not pick up the host's older copy
# (which fails with "version `Qt_6.12' not found"). With WITH_HOST_LIBVA=1 the
# host's libva is added too, so the smoke test can prove the launcher prefers
# the host's libva over the bundled one.
FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
ARG WITH_HOST_LIBVA=0
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 dbus-daemon binutils libstdc++6 libgl1 libegl1 libopengl0 \
    libxkbcommon0 libxcb-cursor0 libxkbcommon-x11-0 fonts-dejavu-core \
    libfontconfig1 libfreetype6 libharfbuzz0b libfribidi0 \
    libpipewire-0.3-0 libpipewire-0.3-common \
    libqt6core6t64 \
    && if [ "$WITH_HOST_LIBVA" = "1" ]; then \
         apt-get install -y --no-install-recommends \
             libva2 libva-drm2 libva-x11-2; \
       fi \
    && rm -rf /var/lib/apt/lists/*
