#!/bin/sh
set -eu

# Production limits, pinned by compilation. The PC suite defines DUMP_PCM_PC
# everywhere, so platform_config.h picks the scaled-down PC values and the
# shipped values are never instantiated. See test/config_contract.c.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CC_BIN=${CC:-cc}

"$CC_BIN" -std=c11 -Wall -Wextra -Werror -I"$SCRIPT_DIR/../src" -fsyntax-only \
    "$SCRIPT_DIR/config_contract.c"

echo "config limits: ok"
