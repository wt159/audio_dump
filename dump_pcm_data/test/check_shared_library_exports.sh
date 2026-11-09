#!/bin/sh
set -eu

OUT_DIR=/tmp/audio_dump_shared_export_check
SO_PATH="$OUT_DIR/libaudiohal_pcm_dump.so"
EXPORTS_PATH="$OUT_DIR/exports.txt"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

cc -shared -fPIC -fvisibility=hidden -std=c11 -Iinclude -Isrc -DDUMP_PCM_PC -D_POSIX_C_SOURCE=200809L \
    src/logger.c src/queue.c src/session.c src/file_mgr.c src/dump_control.c src/dump_writer.c src/sidecar.c src/dump_pcm_data.c \
    -lpthread -o "$SO_PATH"

nm -D --defined-only "$SO_PATH" > "$EXPORTS_PATH"

grep -q ' dump_pcm_data_write$' "$EXPORTS_PATH"
grep -q ' dump_pcm_data_stop$' "$EXPORTS_PATH"

if grep -q ' dump_pcm_data_get_status$' "$EXPORTS_PATH"; then
    printf '%s\n' 'unexpected export: dump_pcm_data_get_status'
    exit 1
fi

if grep -q ' dump_pcm_data_get_active_sessions$' "$EXPORTS_PATH"; then
    printf '%s\n' 'unexpected export: dump_pcm_data_get_active_sessions'
    exit 1
fi
