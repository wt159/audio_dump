#!/bin/sh
set -eu

grep -q '^check-contracts: check-verification-flow check-header-contract check-docs-build-contract check-shared-library-exports$' Makefile
grep -q '^run: check-contracts $(TARGET)$' Makefile
grep -q '^coverage: check-contracts all$' Makefile

# The two compilation contracts must run with the compiler the build used, not
# whatever `cc` happens to be first on PATH: `make CC=<cross compiler>` would
# otherwise build the module with one compiler and check the headers and the
# production limits with another. Pinned here because the pass-through is one
# word in the Makefile and drifts back silently. check_shared_library_exports.sh
# is deliberately excluded - it inspects ELF symbols and must stay on host cc.
grep -q '^check-header-contract:$' Makefile
grep -q 'CC="\$(CC)" sh test/check_verification_flow.sh' Makefile
grep -q 'CC="\$(CC)" sh test/check_header_contract.sh' Makefile
grep -q 'CC_BIN=${CC:-cc}' test/check_config_limits.sh
grep -q 'CC_BIN=${CC:-cc}' test/check_header_contract.sh

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

# Structural, so it runs before the rest: a check script nobody executes is a
# gate that never runs, however green it would have been.
sh "$SCRIPT_DIR/check_contract_inventory.sh"

# Stage-2 lock-order invariants are checked as part of the same contract pass,
# so every make run / make coverage (and the CI PC job) enforces them.
sh "$SCRIPT_DIR/check_lock_order.sh"

# The section 13.2 blocking-amplification probe is a standalone timing-based
# diagnostic, not a CI gate. Keep its in-tree constants pinned to the spec so it
# cannot silently drift from the four-group / two-face contract.
sh "$SCRIPT_DIR/check_blocking_probe.sh"

# Coverage truth lives in docs/test-matrix.md; the production limits are
# compiled on their own because the PC suite never instantiates them.
sh "$SCRIPT_DIR/check_test_matrix.sh"
sh "$SCRIPT_DIR/check_config_limits.sh"

# The matrix is only the source of truth if nothing else states a different
# count. Prose is not rebuilt with the code, so the number it carries is checked
# against the table here rather than trusted.
sh "$SCRIPT_DIR/check_doc_case_counts.sh"

# The ratchet classifies a drop as a regression or as a change of shape, and
# neither situation can be produced by the real suite (the first would mean
# deleting coverage, the second needs a different compiler). The fixture
# self-test is the only thing that keeps those three answers distinct, and the
# re-baseline knob is the only sanctioned way to move the target.
sh "$SCRIPT_DIR/check_coverage_ratchet_selftest.sh"
grep -q 'COVERAGE_REBASELINE' test/check_coverage_ratchet.sh
