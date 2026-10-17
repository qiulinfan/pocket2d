# Cross-compiling Linux x86_64 runtimes with Ubuntu's GCC 14 cross compiler
# (tools/runtime-images/linux.Dockerfile), against the x86_64 multiarch
# development packages.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-linux-gnu-gcc-14)
set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++-14)
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
