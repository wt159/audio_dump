#!/bin/sh
# Windows runtime smoke test.
#
# The Linux harness cannot run on Windows (fork/exec/waitpid), so this builds
# the module with a native MinGW-w64 toolchain and runs a focused smoke that
# covers the platform-specific paths.
#
#   MINGW_BIN=/d/Qt/Tools/mingw730_64/bin sh test/run_windows_smoke.sh
#
# Set USE_CMAKE=1 to build through CMakeLists.txt and link the smoke against the
# resulting static library, which verifies the CMake target on Windows too.
#
# Defaults to the toolchain bundled with Qt 5.14.2 on this workstation.
set -eu

MINGW_BIN=${MINGW_BIN:-/d/Qt/Tools/mingw730_64/bin}
OUT_DIR=${OUT_DIR:-$(pwd)/build/winsmoke}
USE_CMAKE=${USE_CMAKE:-0}
MODULE_DIR=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -x "$MINGW_BIN/gcc" ] && [ ! -x "$MINGW_BIN/gcc.exe" ]; then
    printf 'no gcc under %s; set MINGW_BIN\n' "$MINGW_BIN" >&2
    exit 2
fi

PATH="$MINGW_BIN:$PATH"
export PATH
CC=gcc

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

if [ "$USE_CMAKE" = "1" ]; then
    if [ -n "${CMAKE_BIN:-}" ]; then
        CMAKE=$CMAKE_BIN
    elif command -v cmake >/dev/null 2>&1; then
        CMAKE=cmake
    else
        printf 'no cmake found; set CMAKE_BIN\n' >&2
        exit 2
    fi

    printf 'building through CMake\n'
    "$CMAKE" -S "$MODULE_DIR" -B "$OUT_DIR/cmake" -G "MinGW Makefiles" \
        -DCMAKE_C_COMPILER="$CC" -DDUMP_PCM_DESKTOP_OBSERVE=ON > "$OUT_DIR/cmake.log"
    "$CMAKE" --build "$OUT_DIR/cmake" >> "$OUT_DIR/cmake.log"
    LINK_INPUT="$OUT_DIR/cmake/libaudio_dump.a"
else
    printf 'building with a direct gcc invocation\n'
    mkdir -p "$OUT_DIR/obj"
    for f in "$MODULE_DIR"/src/*.c; do
        printf 'compiling %s\n' "$(basename "$f")"
        "$CC" -std=c11 -Wall -Wextra -I"$MODULE_DIR/include" -I"$MODULE_DIR/src" \
            -DDUMP_PCM_PC -D_POSIX_C_SOURCE=200809L \
            -c "$f" -o "$OUT_DIR/obj/$(basename "$f" .c).o"
    done
    LINK_INPUT="$OUT_DIR"/obj/*.o
fi

# windows_smoke.c enables the debug declarations itself.
"$CC" -std=c11 -Wall -Wextra -I"$MODULE_DIR/include" \
    -o "$OUT_DIR/windows_smoke.exe" "$MODULE_DIR/test/windows_smoke.c" \
    $LINK_INPUT -lwinpthread

ROOT="$OUT_DIR/out"
mkdir -p "$ROOT"
if command -v cygpath >/dev/null 2>&1; then
    ROOT=$(cygpath -w "$ROOT")
fi
ROOT=$(printf '%s\\' "$ROOT")

"$OUT_DIR/windows_smoke.exe" "$ROOT"
