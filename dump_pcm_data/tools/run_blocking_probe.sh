#!/bin/sh
# Build and run the stage-2 section 13.2 blocking-amplification probe.
#
# This is a standalone diagnostic binary, NOT part of dump_pcm_test: it uses its
# own wrap list and the production (default) platform config. The PC config caps
# the queue at 8 blocks, which would turn a 20 ms writer stall into throughput
# drops unrelated to lock contention.
#
# Env: PROBE_LOG=<path>    where the structured output goes (default under build/)
#      PROBE_KEEP=1        keep the run's build/blocking_probe_io/run_<pid> tree
#                          (a failing run keeps it regardless, for inspection)
set -eu

MODULE_DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$MODULE_DIR"

CC_BIN=${CC:-gcc}
MINGW_BIN=${MINGW_BIN:-/d/Qt/Tools/mingw730_64/bin}
if [ -x "$MINGW_BIN/gcc.exe" ] || [ -x "$MINGW_BIN/gcc" ]; then
    PATH="$MINGW_BIN:$PATH"
    export PATH
fi

case "$(uname -s 2>/dev/null || printf unknown)" in
    MINGW*|MSYS*|CYGWIN*) THREAD_LIB=-lwinpthread ;;
    *)                    THREAD_LIB=-lpthread ;;
esac

LOG=${PROBE_LOG:-build/blocking_probe_last.txt}
OUT=build/blocking_probe.exe
mkdir -p build/blocking_probe_io

OBJDIR=build/blocking_probe_obj
mkdir -p "$OBJDIR"

# -fno-builtin-fopen keeps fopen as a real symbol reference so --wrap sees it.
COMMON="-std=c11 -g -O0 -Wall -Wextra -fno-builtin-fopen -I include -I src -D_POSIX_C_SOURCE=200809L"

# DUMP_PCM_PC is both "desktop log sink" and "PC small-queue config". Only
# logger.c needs the former; compiling it separately lets every other
# production TU keep the default (Android/production) config with its 800-block
# queue, 16 sessions and 16 open files. logger.c includes only logger.h and
# platform_config.h, so the split cannot change any shared struct layout.
"$CC_BIN" $COMMON -DDUMP_PCM_PC -c src/logger.c -o "$OBJDIR/logger.o"

set -- src/queue.c src/session.c src/file_mgr.c src/dump_control.c \
       src/dump_writer.c src/sidecar.c src/dump_pcm_data.c \
       tools/lock_block_probe.c
for f in "$@"; do
    "$CC_BIN" $COMMON -c "$f" -o "$OBJDIR/$(basename "$f" .c).o"
done

"$CC_BIN" "$OBJDIR"/*.o \
    -Wl,--wrap=fopen \
    -Wl,--wrap=pthread_mutex_lock \
    -Wl,--wrap=pthread_mutex_unlock \
    "$THREAD_LIB" -o "$OUT"

printf 'built %s; running %d blocks x 4 groups x 2 faces + reverse control\n' "$OUT" 2000
if "$OUT" > "$LOG" 2>&1; then
    rc=0
else
    rc=$?
fi
cat "$LOG"

# One scratch tree per run: build/blocking_probe_io/run_<pid>. Remove exactly the
# directory this run announced, and only when it passed -- a failing run keeps its
# .pcm/.json files for inspection. PROBE_KEEP=1 keeps them either way. The path is
# parsed from the probe's own [config] line and re-matched against the expected
# prefix, so this never becomes a wildcard delete.
SCRATCH=$(sed -n 's/.*root=\(build\/blocking_probe_io\/run_[0-9][0-9]*\).*/\1/p' "$LOG" | head -n 1)
if [ -n "$SCRATCH" ]; then
    if [ "$rc" -eq 0 ] && [ -z "${PROBE_KEEP:-}" ]; then
        case "$SCRATCH" in
            build/blocking_probe_io/run_*)
                if rm -rf -- "$SCRATCH"; then
                    printf 'scratch: removed %s (set PROBE_KEEP=1 to keep it)\n' "$SCRATCH"
                else
                    printf 'scratch: could not remove %s\n' "$SCRATCH"
                fi
                ;;
        esac
    else
        printf 'scratch: kept %s (failed run or PROBE_KEEP set)\n' "$SCRATCH"
    fi
fi
printf 'log: %s\n' "$LOG"
exit "$rc"
