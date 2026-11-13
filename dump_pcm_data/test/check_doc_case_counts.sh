#!/bin/sh
set -eu

# Prose does not get rebuilt with the code. This suite has mis-stated its own
# size three times: 66 in two documents, 67 in an ADR, 69 in the stage-2 record,
# each written while the table held exactly that and then left behind when it
# grew. The matrix is the single source of truth for what is covered; a document
# that restates the count must restate the current one.
#
# Two ways to satisfy this, and no third:
#   - do not restate the count at all (point at docs/test-matrix.md instead);
#   - if the document is a record of a past state, list it in HISTORICAL below
#     with the reason. Its numbers are exempt from comparison but still counted,
#     so the list cannot silently grow stale entries.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
MAIN="$SCRIPT_DIR/main.c"

# Derived from the case table, never written down here: a hard-coded copy would
# be one more place to forget.
LIVE=$(grep -o '{"test_[a-z0-9_]*"' "$MAIN" | sed 's/^{"//; s/"$//' | sort -u | wc -l | tr -d ' ')
if [ "$LIVE" -lt 1 ]; then
    echo "could not read the case table from $MAIN" >&2
    exit 1
fi

# file|reason: documents that describe a state the repository has left.
HISTORICAL='docs/adr/0002-stage2-lock-order-decisions.md|the constraints as decided, when the suite held 67 cases
docs/stage2-lock-order-probe.md|evidence record for PR #55/#56, describing that diff
docs/stage2-lock-order-spec.md|stage-2 acceptance record, closed at 69 cases'

tmp=$(mktemp)
tmp_hits=$(mktemp)
tmp_list=$(mktemp)
tmp_list_seen=$(mktemp)
trap 'rm -f "$tmp" "$tmp_hits" "$tmp_list" "$tmp_list_seen"' EXIT

files=$(git -C "$REPO" ls-files '*.md' 2>/dev/null) || files=''
if [ -z "$files" ]; then
    files=$(cd "$REPO" && find . -name '*.md' -not -path './build/*' -not -path './.git/*' | sed 's|^\./||')
fi

for f in $files; do
    mode=check
    if printf '%s\n' "$HISTORICAL" | cut -d'|' -f1 | grep -qx "$f"; then
        mode=exempt
    fi
    awk -v file="$f" -v live="$LIVE" -v mode="$mode" '
        function report(kind, msg) { printf "%s\t%s\t%s\n", kind, file, msg }
        {
            line = $0
            # "72个用例" and "72 个用例" are the same claim; folding the counter
            # word away keeps the regex ASCII and avoids a quantifier on a
            # multibyte character, which not every awk in the wild accepts.
            gsub(/个/, " ", line)
            hits = 0
            while (match(line, /[0-9]+[ ]*用例/) || match(line, /[0-9]+[ ]+cases?/)) {
                claim = substr(line, RSTART, RLENGTH)
                num = claim
                gsub(/[^0-9]/, "", num)
                hits++
                if (mode == "check" && num + 0 != live + 0) {
                    report("BAD", sprintf("line %d: \"%s\" claims %s cases, the table holds %d", NR, claim, num, live))
                }
                line = substr(line, RSTART + RLENGTH)
            }
            if (hits && mode == "exempt") {
                report("EXEMPT", sprintf("%d", hits))
            }
        }
    ' "$REPO/$f" >> "$tmp"
done

if grep -q '^BAD' "$tmp" 2>/dev/null; then
    echo "documents disagree with the case table ($LIVE cases):" >&2
    sed -n 's/^BAD\t\([^\t]*\)\t\(.*\)/  \1 \2/p' "$tmp" >&2
    echo "state the count only in docs/test-matrix.md, or add the document to HISTORICAL with its reason" >&2
    exit 1
fi

sed -n 's/^EXEMPT\t\([^\t]*\)\t\(.*\)/\1 \2/p' "$tmp" > "$tmp_hits"

# An exemption that no longer covers anything is a list that has quietly rotted:
# the document either lost its count (fine, delete the entry) or the pattern
# above stopped matching it (not fine, the exemption is now doing nothing).
printf '%s\n' "$HISTORICAL" | cut -d'|' -f1 > "$tmp_list"
cut -d' ' -f1 "$tmp_hits" > "$tmp_list_seen"
while read -r f; do
    [ -n "$f" ] || continue
    if ! grep -qxF "$f" "$tmp_list_seen"; then
        echo "HISTORICAL entry with no case count left in it: $f" >&2
        echo "remove the entry if the document no longer restates the count" >&2
        exit 1
    fi
done < "$tmp_list"

exempt_total=0
while read -r f n; do
    [ -n "$f" ] || continue
    exempt_total=$((exempt_total + n))
done < "$tmp_hits"

echo "doc case counts ok: $LIVE cases live, $exempt_total historical references exempt in $(grep -c . "$tmp_list") documents"
