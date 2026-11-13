#!/bin/sh
set -eu

# The matrix is the single source of truth for coverage. This script keeps it
# from drifting: a name in the matrix that no case registers, and a registered
# case that is missing from the matrix, are both hard failures.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
MAIN="$SCRIPT_DIR/main.c"
MATRIX="$REPO/docs/test-matrix.md"

if [ ! -f "$MATRIX" ]; then
    echo "missing docs/test-matrix.md" >&2
    exit 1
fi

tmp_cases=$(mktemp)
tmp_rows=$(mktemp)
trap 'rm -f "$tmp_cases" "$tmp_rows"' EXIT

# The table is grepped by shape rather than by line: one entry spans two lines,
# so a line-anchored pattern would silently miss it.
grep -o '{"test_[a-z0-9_]*"' "$MAIN" | sed 's/^{"//; s/"$//' | sort -u > "$tmp_cases"

grep -E '^\| `test_[a-z0-9_]+`' "$MATRIX" |
    sed -E 's/^\| `(test_[a-z0-9_]+)`.*$/\1/' | sort -u > "$tmp_rows"

stale=$(comm -13 "$tmp_cases" "$tmp_rows")
if [ -n "$stale" ]; then
    echo "docs/test-matrix.md references cases that are not registered:" >&2
    echo "$stale" >&2
    exit 1
fi

missing=$(comm -23 "$tmp_cases" "$tmp_rows")
if [ -n "$missing" ]; then
    echo "cases missing from docs/test-matrix.md:" >&2
    echo "$missing" >&2
    exit 1
fi

if ! awk -F'|' '
    /^\| `test_/ {
        lvl = $4
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", lvl)
        if (lvl != "可失败" && lvl != "弱" && lvl != "恒真" && lvl != "编译期契约" && lvl != "确认不测") {
            printf "bad level in docs/test-matrix.md for %s: [%s]\n", $2, lvl > "/dev/stderr"
            bad = 1
        }
    }
    END { exit bad ? 1 : 0 }
' "$MATRIX"; then
    exit 1
fi

echo "test matrix ok: $(wc -l < "$tmp_cases") cases, all mapped"
