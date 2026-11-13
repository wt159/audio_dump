#!/bin/sh
set -eu

# Production limits, pinned by compilation. The PC suite defines DUMP_PCM_PC
# everywhere, so platform_config.h picks the scaled-down PC values and the
# shipped values are never instantiated. See test/config_contract.c.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CC_BIN=${CC:-cc}

# `make` passes its own $(CC), so this only fails for a direct `sh` invocation on
# a host without a `cc` alias (MinGW has `gcc` and `mingw32-gcc`, no `cc`).
# Say which knob to turn instead of letting the shell report "command not found".
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "config limits: no C compiler named '$CC_BIN' on PATH; set CC=<compiler>" >&2
    exit 1
fi

"$CC_BIN" -std=c11 -Wall -Wextra -Werror -I"$SCRIPT_DIR/../src" -fsyntax-only \
    "$SCRIPT_DIR/config_contract.c"

echo "config limits: ok"
