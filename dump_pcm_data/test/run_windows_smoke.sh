#!/bin/sh
# Windows runtime smoke test.
#
# The Linux harness cannot run on Windows (fork/exec/waitpid), so this builds
# the module with a native MinGW-w64 toolchain and runs a focused smoke that
# covers the platform-specific paths.
#
#   MINGW_BIN=/d/Qt/Tools/mingw730_64/bin sh test/run_windows_smoke.sh
#
# Defaults to the toolchain bundled with Qt 5.14.2 on this workstation.
set -eu

MINGW_BIN=${MINGW_BIN:-/d/Qt/Tools/mingw730_64/bin}
OUT_DIR=${OUT_DIR:-$(pwd)/build/winsmoke}
MODULE_DIR=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -x "$MINGW_BIN/gcc" ] && [ ! -x "$MINGW_BIN/gcc.exe" ]; then
    printf 'no gcc under %s; set MINGW_BIN\n' "$MINGW_BIN" >&2
    exit 2
fi

PATH="$MINGW_BIN:$PATH"
export PATH
CC=gcc

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/obj"

for f in "$MODULE_DIR"/src/*.c; do
    printf 'compiling %s\n' "$(basename "$f")"
    "$CC" -std=c11 -Wall -Wextra -I"$MODULE_DIR/include" -I"$MODULE_DIR/src" \
        -DDUMP_PCM_PC -D_POSIX_C_SOURCE=200809L \
        -c "$f" -o "$OUT_DIR/obj/$(basename "$f" .c).o"
done

"$CC" -std=c11 -Wall -Wextra -I"$MODULE_DIR/include" -DUMP_PCM_INTERNAL_DEBUG_API \
    -o "$OUT_DIR/windows_smoke.exe" "$MODULE_DIR/test/windows_smoke.c" \
    "$OUT_DIR"/obj/*.o -lwinpthread

ROOT="$OUT_DIR/out"
mkdir -p "$ROOT"
if command -v cygpath >/dev/null 2>&1; then
    ROOT=$(cygpath -w "$ROOT")
fi
ROOT=$(printf '%s\\' "$ROOT")

"$OUT_DIR/windows_smoke.exe" "$ROOT"
