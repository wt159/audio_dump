#!/bin/sh
# Static contract for the section 13.2 auxiliary blocking-amplification probe.
#
# The probe is timing-dependent, so it is intentionally NOT a CI gate. This
# contract keeps the in-tree probe from drifting away from the spec constants;
# check_verification_flow.sh runs it inside `make check-contracts`, which the
# Linux PC job executes.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROBE="$ROOT/tools/lock_block_probe.c"
RUNNER="$ROOT/tools/run_blocking_probe.sh"

[ -f "$PROBE" ] || { printf 'blocking probe contract: FAIL (missing %s)\n' "$PROBE" >&2; exit 1; }
[ -f "$RUNNER" ] || { printf 'blocking probe contract: FAIL (missing %s)\n' "$RUNNER" >&2; exit 1; }

check() {
    if ! grep -Fq -- "$1" "$2"; then
        printf 'blocking probe contract: FAIL (missing %s in %s)\n' "$1" "$2" >&2
        exit 1
    fi
}

check '#define SAMPLES 2000' "$PROBE"
check 'static const int g_delays[4] = {0, 20, 0, 20};' "$PROBE"
check '#define LIMIT_BASELINE_US 2000' "$PROBE"
check '#define LIMIT_INJECTED_US 10000' "$PROBE"
check '#define LIMIT_BLOCKED_US 10000' "$PROBE"
check '#define LIMIT_DROPPED_DELTA 20' "$PROBE"
check '#define CALLER_INTERVAL_US 1000' "$PROBE"
check '#define SESSION_INTERVAL_US 100000' "$PROBE"
check 'wait_until(t0 + CALLER_INTERVAL_US)' "$PROBE"
check 'logger_set_level(LOG_LEVEL_ERROR)' "$PROBE"
check 'strcmp(mode, "ab")' "$PROBE"
check 'strcmp(mode, "wb")' "$PROBE"
check 'dump_pcm_set_metadata_enabled(1)' "$PROBE"
check 'file_mgr_mutex_addr_for_test()' "$PROBE"
check 'g_fc_edges' "$PROBE"
check 'count_sidecars' "$PROBE"
check 'sidecars_after <= sidecars_before' "$PROBE"
check '--wrap=fopen' "$RUNNER"
check '--wrap=pthread_mutex_lock' "$RUNNER"
check '--wrap=pthread_mutex_unlock' "$RUNNER"
# Only logger.c may be built in desktop mode: every other production TU keeps
# the default 800-block queue, otherwise the probe measures PC queue drops.
check '-DDUMP_PCM_PC -c src/logger.c' "$RUNNER"

printf 'blocking probe contract: PASS\n'
