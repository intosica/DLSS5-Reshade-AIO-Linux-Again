cmake_minimum_required(VERSION 3.20)
project(dlss5_reshade_aio_addon CXX C)

# ---------------------------------------------------------------------------
# Cross-compiling with mingw-w64 on Linux:
#
#   sudo apt install mingw-w64
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=mingw-w64-x86_64.cmake
#   cmake --build build -j
#
# This mirrors addon/build.bat: two DLLs (nvngx.dll bridge + the
# standalone-dlssnr.addon64 ReShade addon), against the same source layout.
# It does NOT vendor NVIDIA's NGX SDK headers or the DLSS5-Feeder submodule
# (both are required, neither is redistributable / included here) -- set
# FEEDER_ROOT below to wherever you have cloned
# https://github.com/jlrouzies-fr/DLSS5-Feeder (`git submodule update --init`
# from the repo root does this for you) before configuring.
# ---------------------------------------------------------------------------

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "Configure with -DCMAKE_TOOLCHAIN_FILE=mingw-w64-x86_64.cmake "
        "(shipped alongside this CMakeLists.txt) to cross-compile from Linux.")
endif()

set(FEEDER_ROOT "${CMAKE_SOURCE_DIR}/../external/DLSS5-Feeder" CACHE PATH
    "Path to the DLSS5-Feeder submodule checkout (NGX SDK headers, ReShade SDK, imgui, minhook, vulkan headers).")

if(NOT EXISTS "${FEEDER_ROOT}/external/ngx")
    message(FATAL_ERROR
        "FEEDER_ROOT (${FEEDER_ROOT}) does not look populated -- "
        "run 'git submodule update --init --recursive' from the repo root first, "
        "or pass -DFEEDER_ROOT=/path/to/DLSS5-Feeder.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- nvngx.dll: the caller-authenticity bridge ------------------------------
# Built as its own tiny DLL, same as build.bat, because the NGX snippet
# verifies Init_Ext's return address resolves into a module literally named
# nvngx.dll -- keep it a real separate binary, do not fold it into the addon.
add_library(nvngx SHARED
    src/nvngx-bridge.cpp
)
set_target_properties(nvngx PROPERTIES
    OUTPUT_NAME "nvngx"
    PREFIX ""
)
target_link_libraries(nvngx PRIVATE d3d12)
target_compile_options(nvngx PRIVATE -O2 -Wall -Wextra)

# --- proton-compat: Wine detection + NGX resolution + CPU-staging fallback -
add_library(proton-compat STATIC
    src/proton-compat.cpp
)
target_include_directories(proton-compat PUBLIC include)
target_compile_options(proton-compat PRIVATE -O2 -Wall -Wextra -Wno-cast-function-type)

# --- seh-shim: __try/__except replacement (GCC implements neither, in C or
# C++ mode) using AddVectoredExceptionHandler + setjmp/longjmp instead.
# Must be compiled as C -- that's what makes this work without -fms-extensions.
add_library(seh-shim STATIC
    src/seh-shim.c
)
target_include_directories(seh-shim PUBLIC include)
set_source_files_properties(src/seh-shim.c PROPERTIES LANGUAGE C)
target_compile_options(seh-shim PRIVATE -O2 -Wall -Wextra)

# --- standalone-dlssnr.addon64: the ReShade add-on itself -------------------
add_library(standalone-dlssnr SHARED
    src/nr-standalone.cpp
    src/nvof-motion-provider.cpp
    "${FEEDER_ROOT}/external/minhook/src/buffer.c"
    "${FEEDER_ROOT}/external/minhook/src/hook.c"
    "${FEEDER_ROOT}/external/minhook/src/trampoline.c"
    "${FEEDER_ROOT}/external/minhook/src/hde/hde64.c"
)
target_include_directories(standalone-dlssnr PRIVATE
    "${FEEDER_ROOT}/external/ngx"
    "${FEEDER_ROOT}/external/reshade/include"
    "${FEEDER_ROOT}/external/imgui"
    "${FEEDER_ROOT}/external/vulkan"
    "${FEEDER_ROOT}/external/minhook/include"
    include
)
target_link_libraries(standalone-dlssnr PRIVATE
    proton-compat
    seh-shim
    kernel32 user32 d3d9 d3d11 d3d12 dxgi d3dcompiler dcomp
)
set_target_properties(standalone-dlssnr PROPERTIES
    OUTPUT_NAME "standalone-dlssnr"
    SUFFIX ".addon64"
    PREFIX ""
)
target_compile_options(standalone-dlssnr PRIVATE -O2 -g -Wall -Wextra -Wno-unknown-pragmas)

# ReShade addons must export AddonInit/AddonUninit et al -- same def-less
# export mechanism as MSVC's __declspec(dllexport) in the source, so no
# .def file is needed here either.
