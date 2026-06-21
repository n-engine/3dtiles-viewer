cmake_minimum_required(VERSION 3.16)

project(3dtiles_viewer
    VERSION 0.2.0
    DESCRIPTION "Lean Wayland/X11-native 3D Tiles viewer"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_STATIC_MUSL    "Static-link everything (cross-compile to musl target)" OFF)
option(ENABLE_HTTP_LOADER   "Enable libcurl-backed remote tileset loader" ON)
option(ENABLE_WEBP_TEXTURE  "Enable libwebp decoding for EXT_texture_webp" ON)
option(WAYLAND_ONLY         "Hard-fail if Wayland backend is not selected at runtime" OFF)
option(BUILD_TESTS          "Build the unit tests" OFF)

# ---------- System dependencies (pkg-config) --------------------------------
find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)

# GLFW + libepoxy: prefer CMAKE_PREFIX_PATH (build scripts hand us their
# install/ tree there), fall back to system pkg-config for native dev on a
# distro that ships them.
set(_VIEWER_GLFW_LIB    "")
set(_VIEWER_EPOXY_LIB   "")

# GLFW. NO_CMAKE_FIND_ROOT_PATH lets us bypass the Yocto SDK's root-path
# filter so the .a we just built in CMAKE_PREFIX_PATH (outside the sysroot) is
# still discoverable.
find_library(_GLFW_STATIC_LIB NAMES glfw3 PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(_GLFW_STATIC_LIB)
    message(STATUS "Using GLFW from CMAKE_PREFIX_PATH: ${_GLFW_STATIC_LIB}")
    add_library(viewer_glfw INTERFACE)
    target_link_libraries(viewer_glfw INTERFACE ${_GLFW_STATIC_LIB})
    target_include_directories(viewer_glfw INTERFACE ${CMAKE_PREFIX_PATH}/include)
    set(_VIEWER_GLFW_LIB viewer_glfw)
else()
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    set(_VIEWER_GLFW_LIB PkgConfig::GLFW)
endif()

# libepoxy (same NO_CMAKE_FIND_ROOT_PATH escape as GLFW above).
find_library(_EPOXY_STATIC_LIB NAMES epoxy PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(_EPOXY_STATIC_LIB)
    message(STATUS "Using libepoxy from CMAKE_PREFIX_PATH: ${_EPOXY_STATIC_LIB}")
    add_library(viewer_epoxy INTERFACE)
    target_link_libraries(viewer_epoxy INTERFACE ${_EPOXY_STATIC_LIB})
    target_include_directories(viewer_epoxy INTERFACE ${CMAKE_PREFIX_PATH}/include)
    set(_VIEWER_EPOXY_LIB viewer_epoxy)
else()
    pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
    set(_VIEWER_EPOXY_LIB PkgConfig::EPOXY)
endif()

pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

if(ENABLE_HTTP_LOADER)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
endif()

if(ENABLE_WEBP_TEXTURE)
    pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
endif()

# ---------- Vendored header-only libs (third_party/) ------------------------
# These header-only libraries are vendored under third_party/ so the project
# builds against a known-good version regardless of the host distribution.
# If any of them is missing, fail loud -- silently falling back to a system
# install would yield surprising version mismatches.
foreach(_h
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp"
    "${CMAKE_SOURCE_DIR}/third_party/glm/glm/glm.hpp"
    "${CMAKE_SOURCE_DIR}/third_party/stb/stb_image.h"
    "${CMAKE_SOURCE_DIR}/third_party/cgltf/cgltf.h"
)
    if(NOT EXISTS "${_h}")
        message(FATAL_ERROR "Vendored header missing: ${_h}\n"
            "Ensure git submodules / third_party/ are populated.")
    endif()
endforeach()

add_library(viewer_nlohmann_json INTERFACE)
target_include_directories(viewer_nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

add_library(viewer_glm INTERFACE)
target_include_directories(viewer_glm INTERFACE ${CMAKE_SOURCE_DIR}/third_party/glm)

add_library(viewer_stb INTERFACE)
target_include_directories(viewer_stb INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE ${CMAKE_SOURCE_DIR}/third_party/cgltf)

# Dear ImGui (compiled into a static lib so we don't recompile on every change
# of our own translation units).
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
)
target_link_libraries(imgui PUBLIC ${_VIEWER_GLFW_LIB})
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)
# ImGui's bundled `imgui_impl_opengl3_loader.h` resolves GL symbols itself; it
# coexists with libepoxy at runtime (both look up the same driver entries).
target_link_libraries(imgui PUBLIC ${_VIEWER_EPOXY_LIB})

# ---------- Target ----------------------------------------------------------
add_executable(3dtiles-viewer
    src/main.cxx
    src/window.cxx
    src/gpu_caps.cxx
    src/camera.cxx
    src/renderer.cxx
    src/gltf_scene.cxx
    src/tileset.cxx
    src/tile_cache.cxx
    src/touch_ui.cxx
    src/uri.cxx
    src/file_tile_loader.cxx
    $<$<BOOL:${ENABLE_HTTP_LOADER}>:src/http_tile_loader.cxx>
)

target_include_directories(3dtiles-viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(3dtiles-viewer PRIVATE
    ${_VIEWER_GLFW_LIB}
    ${_VIEWER_EPOXY_LIB}
    PkgConfig::EGL
    PkgConfig::WAYLAND_CLIENT
    OpenGL::GL
    viewer_nlohmann_json
    viewer_glm
    viewer_stb
    cgltf
    imgui
    $<$<BOOL:${ENABLE_HTTP_LOADER}>:PkgConfig::CURL>
    $<$<BOOL:${ENABLE_WEBP_TEXTURE}>:PkgConfig::WEBP>
)

target_compile_definitions(3dtiles-viewer PRIVATE
    VIEWER_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${ENABLE_HTTP_LOADER}>:VIEWER_HAS_HTTP=1>
    $<$<BOOL:${ENABLE_WEBP_TEXTURE}>:VIEWER_HAS_WEBP=1>
    $<$<BOOL:${WAYLAND_ONLY}>:VIEWER_WAYLAND_ONLY=1>
)

# NOTE: deliberately NOT -ffast-math. It implies -ffinite-math-only, under which
# the compiler assumes no NaN/Inf and may fold away the isfinite() guards we use
# to reject hostile non-finite values in untrusted .glb / tileset.json data. The
# float math here is not a hot path (the viewer is I/O- and GPU-bound), so the
# trade is all downside.
target_compile_options(3dtiles-viewer PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-O0 -g3>
)

if(BUILD_STATIC_MUSL)
    target_link_options(3dtiles-viewer PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Copy shaders next to the binary on build so the binary can find them at runtime.
add_custom_command(TARGET 3dtiles-viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/data/shaders
        $<TARGET_FILE_DIR:3dtiles-viewer>/shaders
)

install(TARGETS 3dtiles-viewer RUNTIME DESTINATION bin)
install(DIRECTORY data/shaders/ DESTINATION data/shaders)

# ---------- Tests -----------------------------------------------------------
# Pure-logic unit tests (no GL / network / filesystem) so CI can run them on a
# headless runner. Enable with -DBUILD_TESTS=ON.
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_uri
        tests/test_uri.cxx
        src/uri.cxx
    )
    target_include_directories(test_uri PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_uri PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME uri COMMAND test_uri)
endif()
