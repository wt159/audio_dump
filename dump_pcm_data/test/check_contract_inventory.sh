#!/bin/sh
set -eu

# A check script that nothing calls is a gate that never runs, which is the same
# defect as a gate that cannot fail — this repository has spent three rounds on
# the second kind and should not start collecting the first. The directory grows
# by about one file per round, so the wiring is checked rather than remembered:
# every test/check_*.sh must be executed from `make check-contracts`, directly or
# through check_verification_flow.sh, or be listed below with the target that
# runs it instead.
#
# "Executed" means a line that runs it. Naming a script in a comment, or grepping
# its contents from another contract, is not wiring: check_verification_flow.sh
# greps check_coverage_ratchet.sh and check_config_limits.sh without running
# them, and counting those mentions is exactly how an orphan would hide.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
MODULE=$(cd "$SCRIPT_DIR/.." && pwd)
FLOW="$SCRIPT_DIR/check_verification_flow.sh"
MAKE="$MODULE/Makefile"

# file|reason: scripts reachable only through some other target.
EXEMPT='check_coverage_ratchet.sh|runs as `make coverage-ratchet`, because it reads build/coverage/prod/gcov-summary.txt, which only exists after `make coverage`'

closure=$(grep '^check-contracts:' "$MAKE" | sed 's/^check-contracts:[[:space:]]*//')
if [ -z "$closure" ]; then
    echo "no check-contracts prerequisite list in the Makefile" >&2
    exit 1
fi

# Lines that start with the interpreter, so a comment or a grep does not count.
from_flow=$(grep -E '^sh "\$SCRIPT_DIR/check_[a-z_]*\.sh"' "$FLOW" | grep -o 'check_[a-z_]*\.sh' || true)

# Every execution in the Makefile, tagged with the target its recipe belongs to;
# the target is then compared against the check-contracts prerequisite list.
from_make=$(awk '
    /^[a-zA-Z][a-zA-Z0-9_-]*:/ {
        tgt = $0
        sub(/:.*/, "", tgt)
    }
    /sh test\/check_[a-z_]*\.sh/ {
        s = $0
        while (match(s, /check_[a-z_]*\.sh/)) {
            print substr(s, RSTART, RLENGTH) "|" tgt
            s = substr(s, RSTART + RLENGTH)
        }
    }
' "$MAKE")

wired=$(
    {
        printf '%s\n' "$from_flow"
        printf '%s\n' "$from_make" | awk -F'|' -v c="$closure" '
            BEGIN { n = split(c, a, " "); for (i = 1; i <= n; i++) ok[a[i]] = 1 }
            ok[$2] { print $1 }
        '
    } | grep -v '^$' | sort -u || true
)

exempt_list=$(mktemp)
trap 'rm -f "$exempt_list"' EXIT
printf '%s\n' "$EXEMPT" | cut -d'|' -f1 > "$exempt_list"

bad=0
for f in $(cd "$SCRIPT_DIR" && ls check_*.sh); do
    if printf '%s\n' "$wired" | grep -qx "$f"; then
        continue
    fi
    if grep -qx "$f" "$exempt_list"; then
        continue
    fi
    echo "$f is never executed by make check-contracts" >&2
    bad=1
done

# The reverse direction: an exemption for a script that check-contracts now runs,
# or for a script that no longer exists, is a list that has quietly rotted.
while read -r f; do
    [ -n "$f" ] || continue
    if [ ! -f "$SCRIPT_DIR/$f" ]; then
        echo "EXEMPT names a script that does not exist: $f" >&2
        bad=1
    elif printf '%s\n' "$wired" | grep -qx "$f"; then
        echo "EXEMPT lists $f but make check-contracts already runs it" >&2
        bad=1
    fi
done < "$exempt_list"

if [ "$bad" -ne 0 ]; then
    echo "wire it into check_verification_flow.sh (or a check-contracts prerequisite) or list it in EXEMPT with its target" >&2
    exit 1
fi

echo "contract inventory ok: $(ls "$SCRIPT_DIR"/check_*.sh | wc -l | tr -d ' ') scripts, $(printf '%s\n' "$wired" | wc -l | tr -d ' ') executed by check-contracts"
