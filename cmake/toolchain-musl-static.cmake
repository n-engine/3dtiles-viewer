# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# CMake toolchain for fully-static x86_64-linux-musl builds.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-musl-static.cmake \
#         -DMUSL_STATIC_BUILD=ON ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MUSL_CROSS_ROOT "$ENV{HOME}/.local/musl-cross")
set(MUSL_CROSS_PREFIX "x86_64-linux-musl")

set(CMAKE_C_COMPILER   "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-g++")
set(CMAKE_AR           "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-ar")
set(CMAKE_RANLIB       "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-ranlib")
set(CMAKE_STRIP        "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-strip")

set(CMAKE_SYSROOT      "${MUSL_CROSS_ROOT}/${MUSL_CROSS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static link the world.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Force CMake's find_library to only accept .a archives.
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_CROSSCOMPILING_EMULATOR "")

# Sentinel for the project CMakeLists to switch into static-mode link rules.
set(MUSL_STATIC_BUILD ON CACHE BOOL "Build fully-static binary against vendored deps" FORCE)

message(STATUS "=== musl-cross static toolchain ===")
message(STATUS "C   compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "Sysroot:      ${CMAKE_SYSROOT}")
message(STATUS "Linker:       ${CMAKE_EXE_LINKER_FLAGS_INIT}")
