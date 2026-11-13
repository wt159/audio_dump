#!/bin/sh
set -eu

# Coverage ratchet: per-file line coverage may not fall below the committed
# baseline by more than the tolerance. A zero-tolerance ratchet does not work
# here — the denominator moves with the optimisation level and the gcc version
# (the same queue.c reads 82.14% of 84 at -O0 and 86.49% of 74 at -O2), and a
# multi-threaded suite does not hit exactly the same lines every run.
#
# So the comparison has two axes, and they mean different things:
#
#   same denominator, lower permille  -> a regression: lines that used to be
#                                        hit no longer are.
#   different denominator             -> the measurement changed shape. Either
#                                        the TU gained or lost executable lines,
#                                        or a different compiler/flags produced
#                                        the report. Re-baselining is the right
#                                        response to the second case only.
#
# The baseline stores permille and the executable-line count, never the .gcov
# body: that body carries Runs/Graph/Data lines that change on every run.
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

# Reported on every run, pass or fail: when a re-baseline question comes up, the
# first thing worth knowing is which gcov produced the numbers being compared.
GCOV_VERSION=$(gcov --version 2>/dev/null | head -1 || true)
[ -n "$GCOV_VERSION" ] || GCOV_VERSION="unknown gcov"

if [ "${COVERAGE_REBASELINE:-0}" = "1" ]; then
    cp "$BASELINE" "$BASELINE.pre-rebaseline" 2>/dev/null || true
    {
        echo "# Coverage ratchet baseline — see docs/test-gap-spec.md §6.2."
        echo "# Format: <source file> <lines executed, permille> <executable lines>."
        echo "# Calibrated against: $GCOV_VERSION"
        echo "# Regenerate with COVERAGE_REBASELINE=1 sh test/check_coverage_ratchet.sh"
        echo "# after the toolchain or the code changed shape — never to silence a"
        echo "# same-denominator regression."
        cat "$tmp"
    } > "$BASELINE"
    echo "coverage ratchet: re-baselined against $GCOV_VERSION"
    if [ -f "$BASELINE.pre-rebaseline" ]; then
        echo "  previous baseline kept at $BASELINE.pre-rebaseline (delete it, it is not committed)"
    fi
    echo "  commit $BASELINE so the gate compares against the new numbers"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "coverage ratchet: no baseline yet ($BASELINE). Current numbers:"
    echo "# <file> <permille> <executable lines>"
    cat "$tmp"
    echo "coverage ratchet: commit those lines as the baseline to switch the gate on"
    exit 0
fi

if ! output=$(awk -v tol="$TOL" '
    $1 ~ /^#/ { next }
    FNR == NR { base_p[$1] = $2; base_l[$1] = $3; next }
    { cur_p[$1] = $2; cur_l[$1] = $3 }
    END {
        bad = 0
        for (f in base_p) {
            if (!(f in cur_p)) {
                printf "ratchet: %s vanished from the coverage report\n", f
                bad = 1
                continue
            }
            dropped = cur_p[f] + tol < base_p[f]
            reshaped = cur_l[f] != base_l[f]
            if (dropped && reshaped) {
                printf "ratchet: %s changed shape AND fell: %d -> %d executable lines, %d -> %d permille\n", \
                    f, base_l[f], cur_l[f], base_p[f], cur_p[f]
                printf "  if the toolchain or the flags changed, re-baseline; if this TU gained code, the new lines are untested\n"
                bad = 1
            } else if (dropped) {
                lost = int((base_p[f] - cur_p[f]) * base_l[f] / 1000 + 0.5)
                printf "ratchet: %s regressed: %d of %d executable lines stopped being hit (%d -> %d permille)\n", \
                    f, lost, base_l[f], base_p[f], cur_p[f]
                bad = 1
            } else if (reshaped) {
                printf "note: %s changed shape but held: %d -> %d executable lines, %d -> %d permille\n", \
                    f, base_l[f], cur_l[f], base_p[f], cur_p[f]
            }
        }
        for (f in cur_p) {
            if (!(f in base_p)) {
                printf "ratchet: %s is not in the baseline (%d permille)\n", f, cur_p[f]
            }
        }
        exit bad
    }
' "$BASELINE" "$tmp"); then
    if [ -n "$output" ]; then printf '%s\n' "$output" >&2; fi
    echo "coverage ratchet: FAILED (tolerance ${TOL} permille, $GCOV_VERSION)" >&2
    echo "  a same-denominator drop is a regression and must be fixed, not re-baselined" >&2
    echo "  a denominator change after a compiler/flags change is a re-baseline event:" >&2
    echo "    COVERAGE_REBASELINE=1 sh test/check_coverage_ratchet.sh" >&2
    exit 1
fi

if [ -n "$output" ]; then printf '%s\n' "$output"; fi
echo "coverage ratchet: ok ($GCOV_VERSION)"
