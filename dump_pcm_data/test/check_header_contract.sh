#!/bin/sh
set -eu

# Compile with the same compiler the build uses. `make` passes its $(CC) down;
# defaulting to `cc` keeps a bare `sh` invocation working on hosts that have the
# alias, and the guard below names the knob instead of letting the shell report
# "command not found" - or worse, letting the first case (which expects a
# failure) read a missing compiler as a passing negative control.
CC_BIN=${CC:-cc}
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "header contract: no C compiler named '$CC_BIN' on PATH; set CC=<compiler>" >&2
    exit 1
fi

rm -f /tmp/header_external_visibility.o /tmp/header_internal_visibility.o

if "$CC_BIN" -Wall -Wextra -Werror -std=c11 -Iinclude -c test/header_external_visibility.c -o /tmp/header_external_visibility.o; then
    printf '%s\n' "unexpected success: external header still exposes debug query APIs"
    exit 1
fi

"$CC_BIN" -Wall -Wextra -Werror -std=c11 -Iinclude -c test/header_internal_visibility.c -o /tmp/header_internal_visibility.o

# Desktop builds opt in explicitly; the default build stays closed.
"$CC_BIN" -Wall -Wextra -Werror -std=c11 -DDUMP_PCM_DESKTOP_OBSERVE -Iinclude \
    -c test/header_desktop_observe.c -o /tmp/header_desktop_observe.o
