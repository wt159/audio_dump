#!/bin/sh
set -eu

# Coverage ratchet: per-file line coverage may not fall below the committed
# baseline by more than the tolerance. A zero-tolerance ratchet does not work
# here — the denominator moves with the optimisation level and the gcc version
# (the same queue.c reads 82.14% of 84 at -O0 and 86.49% of 74 at -O2), and a
# multi-threaded suite does not hit exactly the same lines every run.
#
# The baseline stores permille, not percentages, and never the .gcov body: that
# body carries Runs/Graph/Data lines that change on every run.
#
# The numbers come from gcov's stdout, which `make coverage` saves next to the
# .gcov files. They cannot come from the .gcov body: gcov only writes the
# `File '...'` / `Lines executed:` summary into the body on gcc 7 — from gcc 8
# on it goes to stdout alone, so a body-based parser reads zero rows on CI and
# passes locally.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
MODULE_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
GCOV_DIR=${GCOV_DIR:-$MODULE_DIR/build/coverage/prod}
SUMMARY=${SUMMARY:-$GCOV_DIR/gcov-summary.txt}
BASELINE=${BASELINE:-$SCRIPT_DIR/coverage_baseline.txt}
TOL=${COVERAGE_TOLERANCE_PERMILLE:-5}

# Only the eight production translation units count. Headers are excluded:
# key_utils.h has a denominator of 36 when the eight TUs are requested together
# and 8 when one of them is, so its percentage is not a property of the header.
PROD_SRC=${PROD_SRC:-"src/logger.c src/queue.c src/session.c src/file_mgr.c src/dump_control.c src/dump_writer.c src/sidecar.c src/dump_pcm_data.c"}
PROD_COUNT=$(printf '%s\n' $PROD_SRC | wc -l | tr -d ' ')

if [ "${COVERAGE_RATCHET:-1}" = "0" ]; then
    echo "coverage ratchet: skipped (COVERAGE_RATCHET=0); this is for local runs only" >&2
    exit 0
fi

if [ ! -f "$SUMMARY" ]; then
    echo "coverage ratchet: $SUMMARY is missing; run make coverage first" >&2
    exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# gcov -f prints a per-function `Lines executed:` line for every function and a
# per-file one after each `File '<path>'` header, so the previous line is the
# guard that keeps the function rows out. The trailing summary line carries no
# File header and is therefore dropped too.
awk -v keep="$PROD_SRC" '
    BEGIN {
        n = split(keep, k, " ")
        for (i = 1; i <= n; i++) want[k[i]] = 1
    }
    prev ~ /^File .+$/ && /^Lines executed:/ {
        path = prev
        sub(/^File \047/, "", path)
        sub(/\047$/, "", path)
        if (path in want) {
            pct = $2
            sub(/^executed:/, "", pct)
            sub(/%$/, "", pct)
            printf "%s %d %d\n", path, pct * 10 + 0.5, $4 + 0
        }
    }
    { prev = $0 }
' "$SUMMARY" | sort > "$tmp"

got=$(wc -l < "$tmp" | tr -d ' ')
if [ "$got" -ne "$PROD_COUNT" ]; then
    echo "coverage ratchet: expected $PROD_COUNT production files in $SUMMARY, parsed $got" >&2
    printf '%s\n' $PROD_SRC | while read -r f; do
        grep -q "^$f " "$tmp" || echo "  missing: $f" >&2
    done
    exit 1
fi

if [ ! -f "$BASELINE" ]; then
    echo "coverage ratchet: no baseline yet ($BASELINE). Current numbers:"
    echo "# <file> <permille> <executable lines>"
    cat "$tmp"
    echo "coverage ratchet: commit those lines as the baseline to switch the gate on"
    exit 0
fi

if ! awk -v tol="$TOL" '
    $1 ~ /^#/ { next }
    NR == FNR { base[$1] = $2; next }
    { cur[$1] = $2 }
    END {
        bad = 0
        for (f in base) {
            if (!(f in cur)) {
                printf "ratchet: %s vanished from the coverage report\n", f
                bad = 1
                continue
            }
            if (cur[f] + tol < base[f]) {
                printf "ratchet: %s fell from %d to %d permille\n", f, base[f], cur[f]
                bad = 1
            }
        }
        for (f in cur) {
            if (!(f in base)) {
                printf "ratchet: %s is not in the baseline (%d permille)\n", f, cur[f]
            }
        }
        exit bad
    }
' "$BASELINE" "$tmp"; then
    echo "coverage ratchet: FAILED (tolerance ${TOL} permille)" >&2
    exit 1
fi

echo "coverage ratchet: ok"
