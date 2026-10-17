# Linux runtime kits (tools/build_runtime.py linux): x86_64, cross-compiled by
# GCC 14 from a native container (no CPU emulation), against the x86_64
# development packages of SDL3's X11, Wayland and audio backends (SDL loads
# those libraries at run time), and a CMake new enough for the engine.
# Ubuntu 24.04, so players need glibc 2.39 or newer.
FROM ubuntu:24.04
RUN ARCH=$(dpkg --print-architecture) \
    && if [ "$ARCH" != amd64 ]; then \
         sed -i "s/^Types: deb$/Types: deb\nArchitectures: $ARCH/" /etc/apt/sources.list.d/ubuntu.sources \
         && printf 'Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main universe\nArchitectures: amd64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' > /etc/apt/sources.list.d/amd64.sources \
         && dpkg --add-architecture amd64; fi \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++-14-x86-64-linux-gnu gcc-14-x86-64-linux-gnu ninja-build git ca-certificates python3 python3-pip \
        pkg-config wayland-protocols libwayland-bin \
        libx11-dev:amd64 libxext-dev:amd64 libxrandr-dev:amd64 libxcursor-dev:amd64 libxi-dev:amd64 \
        libxss-dev:amd64 libxfixes-dev:amd64 libxtst-dev:amd64 libxkbcommon-dev:amd64 libwayland-dev:amd64 \
        libdecor-0-dev:amd64 libegl-dev:amd64 libgl-dev:amd64 libgles-dev:amd64 libasound2-dev:amd64 \
        libpulse-dev:amd64 libpipewire-0.3-dev:amd64 libdbus-1-dev:amd64 libudev-dev:amd64 libdrm-dev:amd64 \
        libgbm-dev:amd64 \
    && pip3 install --break-system-packages cmake==3.31.6 \
    && rm -rf /var/lib/apt/lists/*
ENV PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig
