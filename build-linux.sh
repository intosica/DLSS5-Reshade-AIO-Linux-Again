#!/usr/bin/env bash
# Cross-compiles the addon on Linux with mingw-w64, mirroring addon/build.bat.
#
# Prereqs:
#   sudo apt install mingw-w64
#   git submodule update --init --recursive   # populates external/DLSS5-Feeder
#
# Run from the addon/ directory: ./build-linux.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

FEEDER_ROOT="${FEEDER_ROOT:-$HERE/../external/DLSS5-Feeder}"
if [[ ! -d "$FEEDER_ROOT/external/ngx" ]]; then
    echo "error: $FEEDER_ROOT/external/ngx not found." >&2
    echo "       Run 'git submodule update --init --recursive' from the repo root," >&2
    echo "       or set FEEDER_ROOT=/path/to/DLSS5-Feeder before running this script." >&2
    exit 1
fi

MINGXX=x86_64-w64-mingw32-g++
BUILD="$HERE/build"
mkdir -p "$BUILD"

echo "== nvngx.dll (caller-authenticity bridge) =="
"$MINGXX" -std=c++17 -O2 -Wall -Wextra -shared -municode \
    src/nvngx-bridge.cpp \
    -o "$BUILD/nvngx.dll" \
    -Wl,--out-implib,"$BUILD/libnvngx.a" \
    -ld3d12

echo "== proton-compat (Wine detection / NGX resolution / CPU-staging fallback) =="
"$MINGXX" -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type -c \
    src/proton-compat.cpp \
    -Iinclude \
    -o "$BUILD/proton-compat.o"

echo "== standalone-dlssnr.addon64 =="
"$MINGXX" -std=c++20 -O2 -Wall -Wextra -Wno-unknown-pragmas -shared -municode \
    -I"$FEEDER_ROOT/external/ngx" \
    -I"$FEEDER_ROOT/external/reshade/include" \
    -I"$FEEDER_ROOT/external/imgui" \
    -I"$FEEDER_ROOT/external/vulkan" \
    -I"$FEEDER_ROOT/external/minhook/include" \
    -Iinclude \
    src/nr-standalone.cpp \
    src/nvof-motion-provider.cpp \
    "$BUILD/proton-compat.o" \
    "$FEEDER_ROOT/external/minhook/src/buffer.c" \
    "$FEEDER_ROOT/external/minhook/src/hook.c" \
    "$FEEDER_ROOT/external/minhook/src/trampoline.c" \
    "$FEEDER_ROOT/external/minhook/src/hde/hde64.c" \
    -o "$BUILD/standalone-dlssnr.addon64" \
    -lkernel32 -luser32 -ld3d9 -ld3d11 -ld3d12 -ldxgi -ld3dcompiler -ldcomp \
    -static -static-libgcc -static-libstdc++

echo "== runtime files =="
for f in nvngx_dlssnr.dll nvngx_dlss.dll nvngx_dlssg.dll; do
    if [[ -f "../runtime/$f" ]]; then
        cp "../runtime/$f" "$BUILD/$f"
    else
        echo "WARNING: ../runtime/$f is absent; it will not be included in the package."
    fi
done
cp shaders/DLSS5_AIO_Feed.fx "$BUILD/" 2>/dev/null || true
cp shaders/StandaloneBoundary.fx "$BUILD/" 2>/dev/null || true

echo "Build complete: $BUILD"
