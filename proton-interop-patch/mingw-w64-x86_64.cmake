# Toolchain file: cross-compile Windows x86_64 binaries from Linux with mingw-w64.
# Usage: cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=mingw-w64-x86_64.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER   NAMES ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER  NAMES ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# wmain/WinMain entry points used by ReShade/imgui code paths need -municode
# for the DLL targets that reference them transitively.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -municode" CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static -static-libgcc -static-libstdc++" CACHE STRING "")
