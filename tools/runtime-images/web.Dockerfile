# Web runtime kits (tools/build_runtime.py web): Emscripten.
FROM emscripten/emsdk:4.0.9
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ninja-build \
    && pip3 install --break-system-packages cmake==3.31.6 || pip3 install cmake==3.31.6 \
    && rm -rf /var/lib/apt/lists/*
