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

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
GCOV_DIR=${GCOV_DIR:-build/coverage/prod}
BASELINE=${BASELINE:-$SCRIPT_DIR/coverage_baseline.txt}
TOL=${COVERAGE_TOLERANCE_PERMILLE:-5}

if [ ! -d "$GCOV_DIR" ]; then
    echo "coverage ratchet: $GCOV_DIR is missing; run make coverage first" >&2
    exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# gcov -f prints a per-function "Lines executed:" line for every function and a
# per-file one after the "File '...'" header, so the previous line is the guard
# that keeps the function rows out.
cat "$GCOV_DIR"/*.gcov 2>/dev/null | awk '
    prev ~ /^File .*\.c.$/ && /^Lines executed:/ {
        split(prev, a, "\047")
        pct = $2
        sub(/^executed:/, "", pct)
        sub(/%$/, "", pct)
        printf "%s %d %d %d\n", a[2], pct * 10 + 0.5, $4 + 0, $4 + 0
    }
    { prev = $0 }
' | sort > "$tmp"

# Only the eight production translation units count. Headers are excluded: their
# denominators change with the set of TUs that include them.
awk '$1 ~ /\/src\/[a-z_]+\.c$|^src\/[a-z_]+\.c$/' "$tmp" > "$tmp.prod" || true

if [ ! -s "$tmp.prod" ]; then
    echo "coverage ratchet: no production .gcov files found in $GCOV_DIR" >&2
    cat "$tmp" >&2
    exit 1
fi

if [ ! -f "$BASELINE" ]; then
    echo "coverage ratchet: no baseline yet ($BASELINE). Current numbers (permille):"
    cat "$tmp.prod"
    echo "coverage ratchet: commit those lines as the baseline to switch the gate on"
    exit 0
fi

if ! awk -v tol="$TOL" '
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
' "$BASELINE" "$tmp.prod"; then
    echo "coverage ratchet: FAILED" >&2
    exit 1
fi

echo "coverage ratchet: ok"
