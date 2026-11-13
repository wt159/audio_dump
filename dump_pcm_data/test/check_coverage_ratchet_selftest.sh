#!/bin/sh
set -eu

# The ratchet now tells three situations apart, and a gate is only worth its
# exit code if each of them is reachable on demand. Those situations cannot be
# produced by the real suite (a same-denominator regression would mean deleting
# test coverage, and a denominator change needs a different compiler), so the
# fixtures below synthesise gcov's stdout and the baseline instead.
#
# Every case states the exit code and a substring that must appear in the
# output, so a case fails when the classification regresses into a bare
# "FAILED" or into a silent pass.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
RATCHET="$SCRIPT_DIR/check_coverage_ratchet.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILS=0

# spec rows: <path> <permille> <executable lines>
base_spec() {
    cat <<'SPEC'
src/logger.c 1000 26
src/queue.c 1000 84
src/session.c 985 134
src/file_mgr.c 909 547
src/dump_control.c 981 107
src/dump_writer.c 945 181
src/sidecar.c 769 199
src/dump_pcm_data.c 971 204
SPEC
}

# gcov -f stdout, in the shape the parser expects: a `File '<path>'` header,
# then one per-file `Lines executed:` line.
write_summary() {
    awk '{printf "File \047%s\047\nLines executed:%.2f%% of %d\n\n", $1, $2 / 10, $3}' "$1" > "$2"
}

write_baseline() {
    { echo "# fixture baseline"; cat "$1"; } > "$2"
}

# set_field <spec-in> <spec-out> <path> <permille> <lines>
set_field() {
    awk -v f="$3" -v p="$4" -v l="$5" '
        $1 == f { $2 = p; $3 = l }
        { print $1, $2, $3 }
    ' "$1" > "$2"
}

run_case() {
    name=$1
    want_rc=$2
    want_msg=$3
    shift 3
    rc=0
    out=$("$@" 2>&1) || rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        printf 'FAIL %s: exit %d, wanted %d\n--- output ---\n%s\n' "$name" "$rc" "$want_rc" "$out" >&2
        FAILS=$((FAILS + 1))
        return 0
    fi
    case "$out" in
        *"$want_msg"*) ;;
        *)
            printf 'FAIL %s: output does not contain %s\n--- output ---\n%s\n' "$name" "$want_msg" "$out" >&2
            FAILS=$((FAILS + 1))
            ;;
    esac
}

base_spec > "$WORK/base.spec"
write_summary "$WORK/base.spec" "$WORK/summary.txt"
write_baseline "$WORK/base.spec" "$WORK/baseline.txt"

# 1. Identical numbers: green, and it says which gcov was compared.
run_case "identical" 0 "coverage ratchet: ok" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 2. Same denominator, 50 permille lower: a regression, and the message counts
#    the executable lines that stopped being hit instead of printing two numbers.
set_field "$WORK/base.spec" "$WORK/regressed.spec" src/logger.c 950 26
write_summary "$WORK/regressed.spec" "$WORK/summary.txt"
run_case "same-denominator drop" 1 "stopped being hit" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"
run_case "drop is not called a shape change" 1 "logger.c regressed" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 3. Denominator and coverage both move: classified as shape, and the failure
#    text names the re-baseline command rather than only the numbers.
set_field "$WORK/base.spec" "$WORK/reshaped.spec" src/file_mgr.c 850 553
write_summary "$WORK/reshaped.spec" "$WORK/summary.txt"
run_case "shape change with a drop" 1 "changed shape AND fell" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"
run_case "shape change points at re-baseline" 1 "COVERAGE_REBASELINE=1" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 4. Denominator moved but coverage held: not a failure, but not silent either.
set_field "$WORK/base.spec" "$WORK/reshaped-ok.spec" src/file_mgr.c 910 553
write_summary "$WORK/reshaped-ok.spec" "$WORK/summary.txt"
run_case "shape change that held" 0 "changed shape but held" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 5. A TU missing from the report is not a coverage number to compare.
write_summary "$WORK/base.spec" "$WORK/summary.txt"
head -7 "$WORK/base.spec" > "$WORK/short.spec"
write_summary "$WORK/short.spec" "$WORK/summary.txt"
run_case "missing TU" 1 "parsed 7" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 6. A baseline entry that no longer appears is named, not silently ignored.
write_summary "$WORK/base.spec" "$WORK/summary.txt"
run_case "vanished TU" 1 "vanished from the coverage report" \
    env PROD_SRC="src/logger.c src/queue.c" SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 7. Re-baselining is an explicit act that rewrites the file, records the gcov
#    version and keeps the old file for the diff.
write_summary "$WORK/base.spec" "$WORK/summary.txt"
run_case "re-baseline" 0 "re-baselined" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/rebaselined.txt" COVERAGE_REBASELINE=1 sh "$RATCHET"
case "$(cat "$WORK/rebaselined.txt")" in
    *"Calibrated against:"*) ;;
    *) printf 'FAIL re-baseline writes no compiler line\n' >&2; FAILS=$((FAILS + 1)) ;;
esac
case "$(grep -c '^src/' "$WORK/rebaselined.txt" || true)" in
    8) ;;
    *) printf 'FAIL re-baseline did not write eight rows\n' >&2; FAILS=$((FAILS + 1)) ;;
esac

# 8. No baseline: bootstrap, print the numbers, do not pretend to compare.
run_case "bootstrap" 0 "no baseline yet" \
    env SUMMARY="$WORK/summary.txt" BASELINE="$WORK/absent.txt" sh "$RATCHET"

# 9. Missing report: name the target that produces it.
run_case "no report" 1 "run make coverage first" \
    env SUMMARY="$WORK/absent.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

# 10. The local escape hatch still exists and says it is local-only.
run_case "skip" 0 "COVERAGE_RATCHET=0" \
    env COVERAGE_RATCHET=0 SUMMARY="$WORK/summary.txt" BASELINE="$WORK/baseline.txt" sh "$RATCHET"

if [ "$FAILS" -ne 0 ]; then
    echo "coverage ratchet selftest: $FAILS case(s) failed" >&2
    exit 1
fi

echo "coverage ratchet selftest: ok (10 cases: 4 classifications, 2 structural, 1 re-baseline, 3 guards)"
