# Windows runtime kits (tools/build_runtime.py windows): cross-compiled with
# llvm-mingw (Clang 20, libc++ with std::jthread, MinGW-w64 headers) for x86_64
# Windows.
FROM mstorsjo/llvm-mingw:20250709
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends python3-pip \
    && (pip3 install --break-system-packages cmake==3.31.6 || pip3 install cmake==3.31.6) \
    && rm -rf /var/lib/apt/lists/*
