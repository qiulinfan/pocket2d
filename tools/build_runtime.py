#!/usr/bin/env python3
"""Builds a runtime kit for `pocket package`: a self-contained Release
runtime for one platform, installed under build/runtime/PLATFORM.

  tools/build_runtime.py mac|linux|windows|web [--jobs N]

mac builds on this macOS host. linux (x86_64, cross-compiled by GCC 14),
windows (x86_64, llvm-mingw) and web (Emscripten) build in Docker images from
tools/runtime-images, natively on the host's CPU (no emulation), with the
repository mounted at /src; Docker must be running. Builds are
incremental under build/runtime-build/PLATFORM.
"""
import argparse
import os
import platform as host_platform
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
OPTIONS = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPOCKET_DISTRIBUTION=ON", "-DBUILD_TESTING=OFF",
           "-DPOCKET_ENABLE_GPU_RENDER_TESTS=OFF", "-DPOCKET_ENABLE_IMGUI_TEST_ENGINE=OFF",
           "-DPOCKET_ENABLE_TYPESCRIPT_TESTS=OFF", "-DPOCKET_ENABLE_TRACY=OFF",
           # The engine uses no C++ modules; llvm-mingw's clang-scan-deps cannot find its own headers.
           "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"]
DOCKER = {"linux": (None, ["-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchains/linux-x86_64.cmake"]),
          "windows": (None, ["-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchains/llvm-mingw-x86_64.cmake"]),
          "web": (None, [])}


def run(command, **kwargs):
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def build_mac(jobs):
    if sys.platform != "darwin":
        raise SystemExit("mac runtimes build on a macOS host")
    build, kit = REPOSITORY / "build/runtime-build/mac", REPOSITORY / "build/runtime/mac"
    run(["cmake", "-S", str(REPOSITORY), "-B", str(build)] + OPTIONS)
    run(["cmake", "--build", str(build), "--target", "game_engine", "-j", str(jobs)])
    shutil.rmtree(kit, ignore_errors=True)
    run(["cmake", "--install", str(build), "--prefix", str(kit), "--component", "runtime_kit"])


def build_docker(name, jobs):
    image = "pocket2d-runtime-" + name
    arch, extra = DOCKER[name]
    platform_args = ["--platform", arch] if arch else []
    run(["docker", "build"] + platform_args + ["-t", image, "-f", str(REPOSITORY / "tools/runtime-images" /
                                                                   (name + ".Dockerfile")),
                                               str(REPOSITORY / "tools/runtime-images")])
    build, kit = "/src/build/runtime-build/" + name, "/src/build/runtime/" + name
    configure = ["cmake", "-S", "/src", "-B", build] + OPTIONS + extra
    if name == "web":
        configure = ["emcmake"] + configure
    script = " && ".join([
        " ".join(configure),
        "cmake --build %s --target game_engine -j %d" % (build, jobs),
        "rm -rf %s" % kit,
        "cmake --install %s --prefix %s --component runtime_kit" % (build, kit),
    ])
    if name == "windows":   # llvm-mingw's C++ runtime DLLs ship beside the game
        script += " && cp $(dirname $(command -v x86_64-w64-mingw32-clang))/../x86_64-w64-mingw32/bin/" \
                  "{libc++.dll,libunwind.dll} %s/" % kit
    user = ["-u", "%d:%d" % (os.getuid(), os.getgid())] if hasattr(os, "getuid") else []
    run(["docker", "run", "--rm"] + platform_args + user + ["-e", "HOME=/tmp", "-v", "%s:/src" % REPOSITORY,
                                                             "-w", "/src", image, "bash", "-c", script])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("platform", choices=["mac", "linux", "windows", "web"])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    options = parser.parse_args()
    if options.platform == "mac":
        build_mac(options.jobs)
    else:
        build_docker(options.platform, options.jobs)
    print("runtime kit: %s" % (REPOSITORY / "build/runtime" / options.platform))


if __name__ == "__main__":
    main()
