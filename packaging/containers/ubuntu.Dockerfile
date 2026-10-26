FROM ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 dbus-daemon binutils file \
    qt6-base-dev qt6-declarative-dev qt6-multimedia-dev qt6-wayland \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
    qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtquick-templates \
    qml6-module-qtquick-effects qml6-module-qtmultimedia qml6-module-qtqml-workerscript \
    qt6-svg-plugins libpipewire-0.3-common pipewire-bin \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libpipewire-0.3-dev libva-dev libvulkan-dev libdrm-dev libegl-dev libgbm-dev \
    dpkg-dev desktop-file-utils appstream
