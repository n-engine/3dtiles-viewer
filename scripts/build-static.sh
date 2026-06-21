#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Build the 3D Tiles viewer as a fully-static x86_64 binary against musl.
#
# Phases:
#   deps     build & install libffi, wayland, xkbcommon, libepoxy, glfw
#   project  build the viewer against build-static/install/
#   verify   sanity-check the resulting binary (file/ldd/size)
#   clean    rm -rf build-static
#   all      deps + project + verify (default)
#
# Re-running is idempotent: each dep is skipped if its install marker exists.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${PROJECT_ROOT}/build-static"
INSTALL_DIR="${BUILD_DIR}/install"
PATCH_DIR="${PROJECT_ROOT}/patches"
DIST_DIR="${PROJECT_ROOT}/dist"
TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/toolchain-musl-static.cmake"

MUSL_CROSS_ROOT="${HOME}/.local/musl-cross"
MUSL_CROSS_PREFIX="x86_64-linux-musl"
JOBS="$(nproc)"

# Pinned dep versions.
LIBFFI_VER="3.4.6"
WAYLAND_VER="1.23.1"
WAYLAND_PROTOCOLS_VER="1.36"
XKBCOMMON_VER="1.7.0"
LIBEPOXY_VER="1.5.10"
GLFW_VER="3.4"

log()  { printf '\033[1;34m[build-static]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[build-static]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[build-static]\033[0m %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Phase 0 -- environment
# ---------------------------------------------------------------------------

check_musl_cross() {
    if [ ! -x "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-gcc" ]; then
        err "musl-cross toolchain not found at ${MUSL_CROSS_ROOT}"
        err "fetch a musl-cross toolchain (e.g. musl.cc) into MUSL_CROSS_ROOT"
        exit 1
    fi
    log "musl-cross OK: $(${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-gcc --version | head -1)"
}

check_host_tools() {
    local missing=()
    for t in wayland-scanner meson ninja pkg-config autoconf automake libtool make cmake git wget; do
        command -v "$t" >/dev/null 2>&1 || missing+=("$t")
    done
    if [ ${#missing[@]} -ne 0 ]; then
        err "missing host tools: ${missing[*]}"
        err "sudo apt-get install -y meson ninja-build pkg-config autoconf automake libtool build-essential cmake git wget libwayland-bin"
        exit 1
    fi
    log "host tools OK"
}

setup_environment() {
    export PATH="${MUSL_CROSS_ROOT}/bin:${PATH}"
    export CC="${MUSL_CROSS_PREFIX}-gcc"
    export CXX="${MUSL_CROSS_PREFIX}-g++"
    export AR="${MUSL_CROSS_PREFIX}-ar"
    export RANLIB="${MUSL_CROSS_PREFIX}-ranlib"
    export STRIP="${MUSL_CROSS_PREFIX}-strip"
    export LDFLAGS="-static -static-libgcc -static-libstdc++"
    export CFLAGS="-O2 -fPIC"
    export CXXFLAGS="-O2 -fPIC"

    # pkg-config: search only inside our INSTALL_DIR so we never accidentally
    # pull host .pc files (and their dynamic .so deps).
    export PKG_CONFIG_LIBDIR="${INSTALL_DIR}/lib/pkgconfig:${INSTALL_DIR}/share/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR=""
    # Required so wayland's pkg-config files behave correctly when consumed by
    # downstream meson runs (the host-installed wayland-scanner is still used).
    export PATH="${INSTALL_DIR}/bin:${PATH}"
}

# Generate a meson cross file pointing at the musl toolchain. Re-written on
# every run so version bumps to MUSL_CROSS_ROOT are picked up.
write_meson_cross_file() {
    local f="${BUILD_DIR}/meson-musl.ini"
    mkdir -p "${BUILD_DIR}"
    cat > "${f}" <<EOF
[binaries]
c = '${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-gcc'
cpp = '${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-g++'
ar = '${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-ar'
strip = '${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = false
sys_root = '${MUSL_CROSS_ROOT}/${MUSL_CROSS_PREFIX}'

[built-in options]
c_args = ['-O2', '-fPIC']
cpp_args = ['-O2', '-fPIC']
c_link_args = ['-static']
cpp_link_args = ['-static']
default_library = 'static'
prefix = '${INSTALL_DIR}'
EOF
    echo "${f}"
}

# ---------------------------------------------------------------------------
# Phase 1 -- deps
# ---------------------------------------------------------------------------

fetch_tarball() {
    # fetch_tarball <name> <url> <unpacked-dir-name>
    local name="$1" url="$2" dir="$3"
    if [ -d "${BUILD_DIR}/${dir}" ]; then
        log "${name}: source already present (${dir})"
        return 0
    fi
    log "${name}: fetching ${url}"
    cd "${BUILD_DIR}"
    local tar
    tar="$(basename "${url}")"
    [ -f "${tar}" ] || wget --no-verbose "${url}"
    tar xf "${tar}"
    [ -d "${dir}" ] || { err "${name}: expected dir ${dir} not found after extract"; exit 1; }
}

# --- libffi ---------------------------------------------------------------
build_libffi() {
    [ -f "${INSTALL_DIR}/lib/libffi.a" ] && { log "libffi: already installed"; return 0; }
    log "libffi: building"
    fetch_tarball "libffi" \
        "https://github.com/libffi/libffi/releases/download/v${LIBFFI_VER}/libffi-${LIBFFI_VER}.tar.gz" \
        "libffi-${LIBFFI_VER}"
    cd "${BUILD_DIR}/libffi-${LIBFFI_VER}"
    ./configure \
        --host="${MUSL_CROSS_PREFIX}" \
        --prefix="${INSTALL_DIR}" \
        --disable-shared --enable-static \
        --disable-docs >/dev/null
    make -j"${JOBS}" >/dev/null
    make install >/dev/null
}

# --- wayland (libs + scanner, but we use the host scanner) ----------------
build_wayland() {
    [ -f "${INSTALL_DIR}/lib/libwayland-client.a" ] && { log "wayland: already installed"; return 0; }
    log "wayland: building (client/cursor/egl only, scanner from host)"
    fetch_tarball "wayland" \
        "https://gitlab.freedesktop.org/wayland/wayland/-/releases/${WAYLAND_VER}/downloads/wayland-${WAYLAND_VER}.tar.xz" \
        "wayland-${WAYLAND_VER}"
    local cross_file
    cross_file="$(write_meson_cross_file)"
    local b="${BUILD_DIR}/wayland-${WAYLAND_VER}/build"
    rm -rf "${b}"
    meson setup "${b}" "${BUILD_DIR}/wayland-${WAYLAND_VER}" \
        --cross-file "${cross_file}" \
        --prefix "${INSTALL_DIR}" \
        --default-library=static \
        -Dscanner=false \
        -Dlibraries=true \
        -Dtests=false \
        -Ddocumentation=false \
        -Ddtd_validation=false \
        >/dev/null
    meson compile -C "${b}" >/dev/null
    meson install -C "${b}" >/dev/null
}

# --- wayland-protocols (just XML; arch-independent) -----------------------
build_wayland_protocols() {
    [ -d "${INSTALL_DIR}/share/wayland-protocols/stable" ] && { log "wayland-protocols: already installed"; return 0; }
    log "wayland-protocols: installing XML files"
    fetch_tarball "wayland-protocols" \
        "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/${WAYLAND_PROTOCOLS_VER}/downloads/wayland-protocols-${WAYLAND_PROTOCOLS_VER}.tar.xz" \
        "wayland-protocols-${WAYLAND_PROTOCOLS_VER}"
    local cross_file
    cross_file="$(write_meson_cross_file)"
    local b="${BUILD_DIR}/wayland-protocols-${WAYLAND_PROTOCOLS_VER}/build"
    rm -rf "${b}"
    meson setup "${b}" "${BUILD_DIR}/wayland-protocols-${WAYLAND_PROTOCOLS_VER}" \
        --cross-file "${cross_file}" \
        --prefix "${INSTALL_DIR}" \
        -Dtests=false >/dev/null
    meson install -C "${b}" >/dev/null
}

# --- libxkbcommon (no X11, no XML) ---------------------------------------
build_xkbcommon() {
    [ -f "${INSTALL_DIR}/lib/libxkbcommon.a" ] && { log "xkbcommon: already installed"; return 0; }
    log "xkbcommon: building (--disable-x11)"
    fetch_tarball "xkbcommon" \
        "https://xkbcommon.org/download/libxkbcommon-${XKBCOMMON_VER}.tar.xz" \
        "libxkbcommon-${XKBCOMMON_VER}"
    local cross_file
    cross_file="$(write_meson_cross_file)"
    local b="${BUILD_DIR}/libxkbcommon-${XKBCOMMON_VER}/build"
    rm -rf "${b}"
    # Hard-disable: x11 backend, tools, docs, wayland (we only want the keymap
    # parser -- Wayland client lib is from above, GLFW uses xkbcommon directly).
    meson setup "${b}" "${BUILD_DIR}/libxkbcommon-${XKBCOMMON_VER}" \
        --cross-file "${cross_file}" \
        --prefix "${INSTALL_DIR}" \
        --default-library=static \
        -Denable-x11=false \
        -Denable-wayland=false \
        -Denable-docs=false \
        -Denable-tools=false \
        -Denable-xkbregistry=false \
        >/dev/null
    meson compile -C "${b}" >/dev/null
    meson install -C "${b}" >/dev/null
}

# --- libepoxy (EGL only, no GLX, no X11) ----------------------------------
build_libepoxy() {
    [ -f "${INSTALL_DIR}/lib/libepoxy.a" ] && { log "libepoxy: already installed"; return 0; }
    log "libepoxy: building (EGL only)"
    fetch_tarball "libepoxy" \
        "https://github.com/anholt/libepoxy/releases/download/${LIBEPOXY_VER}/libepoxy-${LIBEPOXY_VER}.tar.xz" \
        "libepoxy-${LIBEPOXY_VER}"
    local cross_file
    cross_file="$(write_meson_cross_file)"
    local b="${BUILD_DIR}/libepoxy-${LIBEPOXY_VER}/build"
    rm -rf "${b}"
    meson setup "${b}" "${BUILD_DIR}/libepoxy-${LIBEPOXY_VER}" \
        --cross-file "${cross_file}" \
        --prefix "${INSTALL_DIR}" \
        --default-library=static \
        -Degl=yes \
        -Dglx=no \
        -Dx11=false \
        -Dtests=false \
        >/dev/null
    meson compile -C "${b}" >/dev/null
    meson install -C "${b}" >/dev/null
}

# --- GLFW 3.4 (Wayland-only, patched to drop dlopen) ----------------------
build_glfw() {
    [ -f "${INSTALL_DIR}/lib/libglfw3.a" ] && { log "glfw: already installed"; return 0; }
    log "glfw: building (Wayland-only, no-dlopen patch)"
    fetch_tarball "glfw" \
        "https://github.com/glfw/glfw/releases/download/${GLFW_VER}/glfw-${GLFW_VER}.zip" \
        "glfw-${GLFW_VER}"

    # Apply our no-dlopen patch, which rewrites each
    # _glfwPlatformLoadModule("libwayland-client.so.0") (and siblings) to
    # _glfwPlatformLoadModule(NULL). On a static binary, dlopen(NULL) returns
    # the main-program handle and dlsym then resolves wl_display_connect etc.
    # from the statically-linked wayland-client.a inside our own ELF.
    if [ -f "${PATCH_DIR}/glfw-no-dlopen.patch" ]; then
        local marker="${BUILD_DIR}/glfw-${GLFW_VER}/.patched-no-dlopen"
        if [ ! -f "${marker}" ]; then
            (cd "${BUILD_DIR}/glfw-${GLFW_VER}" && patch -p1 < "${PATCH_DIR}/glfw-no-dlopen.patch")
            touch "${marker}"
        fi
    else
        warn "glfw: ${PATCH_DIR}/glfw-no-dlopen.patch not found -- wayland symbols will dlopen at runtime and FAIL on a fully-static binary"
    fi

    local b="${BUILD_DIR}/glfw-${GLFW_VER}/build"
    rm -rf "${b}"
    # Tell cmake to find our deps. xkbcommon + wayland-protocols are needed at
    # GLFW configure time (it runs wayland-scanner against the XML files).
    cmake -S "${BUILD_DIR}/glfw-${GLFW_VER}" -B "${b}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DGLFW_BUILD_X11=OFF \
        -DGLFW_BUILD_WAYLAND=ON \
        -DGLFW_BUILD_DOCS=OFF \
        -DGLFW_BUILD_EXAMPLES=OFF \
        -DGLFW_BUILD_TESTS=OFF \
        >/dev/null
    cmake --build "${b}" -j"${JOBS}" >/dev/null
    cmake --install "${b}" >/dev/null
}

build_dependencies() {
    log "==== Phase 1: building dependencies ===="
    mkdir -p "${INSTALL_DIR}"
    build_libffi
    build_wayland
    build_wayland_protocols
    build_xkbcommon
    build_libepoxy
    build_glfw
    log "==== dependencies done ===="
}

# ---------------------------------------------------------------------------
# Phase 2 -- project
# ---------------------------------------------------------------------------

build_project() {
    log "==== Phase 2: building viewer ===="
    local b="${BUILD_DIR}/viewer"
    rm -rf "${b}"
    cmake -S "${PROJECT_ROOT}" -B "${b}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
        -DMUSL_STATIC_BUILD=ON \
        -DENABLE_HTTP_LOADER=OFF
    cmake --build "${b}" -j"${JOBS}"
    mkdir -p "${DIST_DIR}/shaders"
    cp "${b}/3dtiles-viewer" "${DIST_DIR}/"
    cp -r "${PROJECT_ROOT}/src/shaders/." "${DIST_DIR}/shaders/"
    "${MUSL_CROSS_ROOT}/bin/${MUSL_CROSS_PREFIX}-strip" "${DIST_DIR}/3dtiles-viewer"
    log "==== viewer built: ${DIST_DIR}/3dtiles-viewer ===="
}

# ---------------------------------------------------------------------------
# Phase 3 -- verify
# ---------------------------------------------------------------------------

verify() {
    local bin="${DIST_DIR}/3dtiles-viewer"
    [ -x "${bin}" ] || { err "verify: ${bin} not found"; exit 1; }
    log "==== Phase 3: verifying ${bin} ===="
    file "${bin}"
    if ldd "${bin}" 2>&1 | grep -qE "not a dynamic executable|statically linked"; then
        log "ldd: not a dynamic executable -- OK static"
    else
        err "ldd reports dynamic dependencies -- static link did not take"
        ldd "${bin}" || true
        exit 1
    fi
    local sz
    sz="$(du -h "${bin}" | cut -f1)"
    log "size: ${sz}"
}

# ---------------------------------------------------------------------------
# Phase 4 -- clean
# ---------------------------------------------------------------------------

clean() {
    log "removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    log "removing ${DIST_DIR}"
    rm -rf "${DIST_DIR}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

PHASE="${1:-all}"

case "${PHASE}" in
    deps)    check_musl_cross; check_host_tools; setup_environment; build_dependencies ;;
    project) check_musl_cross; check_host_tools; setup_environment; build_project ;;
    verify)  verify ;;
    clean)   clean ;;
    all)     check_musl_cross; check_host_tools; setup_environment;
             build_dependencies; build_project; verify ;;
    *)
        err "usage: $0 {deps|project|verify|clean|all}"
        exit 1
        ;;
esac
