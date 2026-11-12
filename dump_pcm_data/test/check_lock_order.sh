#!/bin/sh
# Stage-2 static lock-order contract (docs/stage2-lock-order-spec.md section 12).
#
# Stage-2 invariants, both slices:
#   L1  F is a leaf lock: capture_* takes/releases F on every path, and no
#       blocking file I/O happens while F (or C) is held.
#   L2  The writer's last F unlock precedes the first publish-phase C lock.
#   L4  Blocking I/O is confined to file_mgr_run_job() / run_jobs(), never
#       while C or F is held.
#   POD jobs must not reach back into session_info_t.
#
# Checks 1-6 cover the file_mgr side (PR 1/2). Checks 7-8 cover the writer /
# public-API side (PR 2/2): no C-held region may call file_mgr_* or do I/O,
# and the execution phase may not acquire C at all.
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

# 7. No C-held region in the production hot paths may call a file_mgr_* entry
#    or perform blocking I/O. check_storage()/queue_*/session_* are pure memory
#    or explicitly out of scope (spec section 15) and are not matched here.
#    pthread_cond_timedwait releases C while waiting; treating that window as
#    C-held is the conservative direction, so it cannot mask a violation.
# Literal substrings, not a regex: awk's dynamic regex would eat the escaped
# parentheses and turn the whole expression into an unbalanced pattern.
HOT_TOKENS='file_mgr_ fopen( fclose( fwrite( fflush( opendir( readdir( remove( rename( sidecar_write_'
for hot in dump_writer.c dump_pcm_data.c; do
    if awk -v toks="$HOT_TOKENS" '
        BEGIN { n = split(toks, tok, " ") }
        /pthread_mutex_lock\(&g_ctx\.mutex\)/ { inC = 1 }
        inC && /pthread_mutex_unlock\(&g_ctx\.mutex\)/ { inC = 0; next }
        inC {
            for (k = 1; k <= n; ++k) {
                if (index($0, tok[k]) > 0) {
                    printf "%s:%d: %s\n", FILENAME, NR, $0 > "/dev/stderr"
                    bad = 1
                }
            }
        }
        END { exit bad ? 1 : 0 }
    ' "$MODULE_DIR/src/$hot"; then
        :
    else
        fail "$hot performs file I/O or calls the compatibility layer while holding C"
    fi
done

# 8. The execution phase and the deferred-close helper must never acquire C:
#    that would turn an F-held region into F->C (the ABBA edge the evidence
#    ticket pinned down). This is the static half of invariant L2.
EXEC_BODY=$(awk '/^static void execute_block\(/,/^}/' "$MODULE_DIR/src/dump_writer.c")
[ -n "$EXEC_BODY" ] || fail "execute_block() not found"
if printf '%s\n' "$EXEC_BODY" | grep -q 'pthread_mutex_lock(&g_ctx.mutex)'; then
    fail "execute_block() acquires C (L2 violation)"
fi
DEFER_BODY=$(awk '/^void run_deferred_close\(/,/^}/' "$MODULE_DIR/src/dump_writer.c")
[ -n "$DEFER_BODY" ] || fail "run_deferred_close() not found"
if printf '%s\n' "$DEFER_BODY" | grep -q 'pthread_mutex_lock(&g_ctx.mutex)'; then
    fail "run_deferred_close() acquires C (L2 violation)"
fi

printf 'lock-order contract: OK (6 capture sites, balanced F, I/O confined to run, no C-held I/O)\n'
