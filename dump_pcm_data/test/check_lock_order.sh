#!/bin/sh
# Stage-2 static lock-order contract (docs/stage2-lock-order-spec.md section 12).
#
# PR 1/2 scope: the file_mgr side of the invariants.
#   L1  F is a leaf lock: capture_* takes/releases F on every path, and no
#       blocking file I/O happens while F (or C) is held.
#   L4  Blocking I/O is confined to file_mgr_run_job() and the two run-side
#       helpers it calls (ensure_dir_exists, run_cleanup).
#   POD jobs must not reach back into session_info_t.
#
# The writer-side "no I/O inside a C-held region" assertion arrives with PR 2/2,
# when the three-phase loop replaces the compatibility layer on hot paths.
set -eu

MODULE_DIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$MODULE_DIR/src/file_mgr.c"
JOB_H="$MODULE_DIR/src/file_mgr_job.h"

fail() {
    printf 'lock-order contract: %s\n' "$1" >&2
    exit 1
}

[ -f "$SRC" ] || fail "missing $SRC"
[ -f "$JOB_H" ] || fail "missing $JOB_H"

# Print one function body: top-level functions in this file put their closing
# brace in column 0, while every nested brace is indented.
extract() {
    sed -n "/^.*file_mgr_$1(/,/^}/p" "$SRC"
}

IO_PATTERN='fopen\(|fclose\(|fwrite\(|fflush\(|opendir\(|readdir\(|remove\(|rename\(|sidecar_write_meta\(|sidecar_write_file\('

CAPTURES="capture_open capture_write capture_close capture_initial_meta capture_cleanup capture_ensure_dir"

# 1. capture bodies are pure memory: no blocking file I/O under F.
for fn in $CAPTURES; do
    body=$(extract "$fn")
    [ -n "$body" ] || fail "capture function not found: file_mgr_$fn"
    if printf '%s\n' "$body" | grep -Eq "$IO_PATTERN"; then
        fail "file_mgr_$fn performs blocking I/O while holding F"
    fi
done

# 2. every capture takes F and releases it (this codebase uses a single exit
#    path per capture, so one lock / one unlock is both necessary and enough).
for fn in $CAPTURES; do
    body=$(extract "$fn")
    locks=$(printf '%s\n' "$body" | grep -c 'pthread_mutex_lock(&g_file_mgr_mutex)' || true)
    unlocks=$(printf '%s\n' "$body" | grep -c 'pthread_mutex_unlock(&g_file_mgr_mutex)' || true)
    [ "$locks" -ge 1 ] || fail "file_mgr_$fn never takes F"
    [ "$locks" = "$unlocks" ] || fail "file_mgr_$fn has unbalanced F: $locks locks, $unlocks unlocks"
done

RUN_BODY=$(extract "run_job")
[ -n "$RUN_BODY" ] || fail "file_mgr_run_job not found"

# 3. run_job never acquires C / S / I. Short F-held accounting blocks are fine.
if printf '%s\n' "$RUN_BODY" | grep -Eq 'g_ctx\.mutex|g_init_mutex|g_storage_mutex'; then
    fail "file_mgr_run_job acquires a non-F module lock"
fi

# 4. the F-held blocks inside run_job stay pure memory.
if printf '%s\n' "$RUN_BODY" | awk '
    /pthread_mutex_lock\(&g_file_mgr_mutex\)/ { inf = 1 }
    inf && /pthread_mutex_unlock\(&g_file_mgr_mutex\)/ { inf = 0 }
    inf && /fopen\(|fclose\(|fwrite\(|fflush\(|opendir\(|remove\(|sidecar_write_/ { bad = 1 }
    END { exit bad ? 1 : 0 }
' ; then
    :
else
    fail "an F-held block inside file_mgr_run_job performs blocking I/O"
fi

# 5. jobs are self-contained PODs: no session back-reference.
if grep -q 'session_info_t\|session_manager_t' "$JOB_H"; then
    fail "job/snapshot types reach back into session internals"
fi

# 6. the legacy compatibility layer contains no direct I/O; it must be a
#    sequence of capture_* + file_mgr_run_job calls only.
COMPAT="init destroy get_handle write close attach_meta is_tracking cleanup"
for fn in $COMPAT; do
    body=$(extract "$fn")
    [ -n "$body" ] || fail "compatibility entry not found: file_mgr_$fn"
    if printf '%s\n' "$body" | grep -Eq "$IO_PATTERN"; then
        fail "compatibility entry file_mgr_$fn performs I/O directly"
    fi
done

printf 'lock-order contract: OK (6 capture sites, balanced F, I/O confined to run)\n'
