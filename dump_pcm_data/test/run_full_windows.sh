#!/bin/sh
# Windows full-suite runner.
#
# Builds the PC harness with a native MinGW-w64 toolchain and runs the full PC suite
# in one process. The Windows smoke script stays as the fast sanity check; this
# one is the real gate.
#
#   MINGW_BIN=/d/Qt/Tools/mingw730_64/bin sh test/run_full_windows.sh
#
# Set USE_CMAKE=1 to build through CMakeLists.txt (-DDUMP_PCM_BUILD_TESTS=ON)
# and run the binary CTest registered, which also exercises the CMake target.
#
# Defaults to the toolchain bundled with Qt 5.14.2 on this workstation; CI
# overrides MINGW_BIN with the image's own toolchain.
set -eu

MINGW_BIN=${MINGW_BIN:-/d/Qt/Tools/mingw730_64/bin}
OUT_DIR=${OUT_DIR:-$(pwd)/build/winfull}
USE_CMAKE=${USE_CMAKE:-0}
MODULE_DIR=$(cd "$(dirname "$0")/.." && pwd)

# Mirrors LDFLAGS in the Makefile. Keep the two lists in step: a symbol that
# silently stops being wrapped turns its fault-injection case into a no-op.
WRAPS="-Wl,--wrap=malloc \
-Wl,--wrap=calloc \
-Wl,--wrap=realloc \
-Wl,--wrap=queue_pop \
-Wl,--wrap=pthread_mutex_init \
-Wl,--wrap=pthread_mutex_lock \
-Wl,--wrap=pthread_mutex_unlock \
-Wl,--wrap=pthread_condattr_init \
-Wl,--wrap=pthread_condattr_setclock \
-Wl,--wrap=pthread_cond_init \
-Wl,--wrap=pthread_cond_timedwait \
-Wl,--wrap=opendir"

if [ ! -x "$MINGW_BIN/gcc" ] && [ ! -x "$MINGW_BIN/gcc.exe" ]; then
    printf 'no gcc under %s; set MINGW_BIN\n' "$MINGW_BIN" >&2
    exit 2
fi

PATH="$MINGW_BIN:$PATH"
export PATH
CC=gcc

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/obj" "$OUT_DIR/root"

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
        -DCMAKE_C_COMPILER="$CC" -DDUMP_PCM_BUILD_TESTS=ON > "$OUT_DIR/cmake.log"
    "$CMAKE" --build "$OUT_DIR/cmake" >> "$OUT_DIR/cmake.log"
    BINARY="$OUT_DIR/cmake/dump_pcm_test.exe"
else
    printf 'building with a direct gcc invocation\n'
    for f in "$MODULE_DIR"/src/*.c; do
        "$CC" -std=c11 -Wall -Wextra -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc \
            -I"$MODULE_DIR/include" -I"$MODULE_DIR/src" \
            -DDUMP_PCM_PC -D_POSIX_C_SOURCE=200809L \
            -c "$f" -o "$OUT_DIR/obj/$(basename "$f" .c).o"
    done
    for f in "$MODULE_DIR"/test/main.c "$MODULE_DIR"/test/whitebox.c "$MODULE_DIR"/test/proc_iso.c; do
        "$CC" -std=c11 -Wall -Wextra -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc \
            -I"$MODULE_DIR/include" -I"$MODULE_DIR/src" \
            -I"$MODULE_DIR/test" -DDUMP_PCM_PC -D_POSIX_C_SOURCE=200809L \
            -c "$f" -o "$OUT_DIR/obj/$(basename "$f" .c).o"
    done
    # shellcheck disable=SC2086
    "$CC" -o "$OUT_DIR/dump_pcm_test.exe" "$OUT_DIR"/obj/*.o $WRAPS -lwinpthread
    BINARY="$OUT_DIR/dump_pcm_test.exe"
fi

# A native binary cannot use the POSIX spelling of the root directory. cygpath
# -m keeps forward slashes, which is what dump_pcm_set_root_dir() normalises.
ROOT="$OUT_DIR/root"
if command -v cygpath >/dev/null 2>&1; then
    ROOT=$(cygpath -m "$ROOT")
fi

printf 'running the full suite against %s\n' "$ROOT"
AUDIO_DUMP_ENABLED=1 AUDIO_DUMP_DIR="$ROOT" "$BINARY"
