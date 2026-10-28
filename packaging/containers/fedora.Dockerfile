FROM fedora:44@sha256:43b29f65a41eb9c35e1cd5323e3bdf3b655c2357a9f4f1ff2f9c2798e5045d80
ENV LANG=C.UTF-8
ARG DEPENDENCY_REFRESH=manual
RUN dnf install -y --setopt=install_weak_deps=False \
    cmake ninja-build ccache gcc-c++ make pkgconf-pkg-config python3 dbus-daemon binutils file cpio \
    qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtmultimedia-devel qt6-qtwayland qt6-qtsvg \
    ffmpeg-free-devel pipewire-devel pipewire-utils libva-devel vulkan-loader-devel vulkan-headers \
    mesa-libEGL-devel mesa-libgbm-devel libdrm-devel rpm-build desktop-file-utils appstream
