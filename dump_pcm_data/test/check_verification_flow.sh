#!/bin/sh
set -eu

grep -q '^check-contracts: check-verification-flow check-header-contract check-docs-build-contract check-shared-library-exports$' Makefile
grep -q '^run: check-contracts $(TARGET)$' Makefile
grep -q '^coverage: check-contracts all$' Makefile

# Stage-2 lock-order invariants are checked as part of the same contract pass,
# so every make run / make coverage (and the CI PC job) enforces them.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
sh "$SCRIPT_DIR/check_lock_order.sh"
