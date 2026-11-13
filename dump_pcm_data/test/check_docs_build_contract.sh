#!/bin/sh
set -eu

grep -q 'BUILD_SHARED_LIBRARY' Android.mk
grep -q 'dump_control.c' Android.mk
grep -q 'dump_writer.c' Android.mk
grep -q '_POSIX_C_SOURCE=200809L' Android.mk
grep -q '共享库' ../docs/design.md
grep -q 'DUMP_PCM_INTERNAL_DEBUG_API' ../docs/design.md
grep -q '共享库' ../docs/plan.md

# The test carriers only work if the three entry documents route to them: a gate
# nobody can find is a gate nobody runs. The inventory must be reachable from all
# three (root README for humans, AGENTS.md for agents, module README for the
# module), and the ratchet target plus its baseline from AGENTS.md.
grep -q 'docs/test-matrix.md' ../AGENTS.md
grep -q 'docs/test-gap-spec.md' ../AGENTS.md
grep -q 'make coverage-ratchet' ../AGENTS.md
grep -q 'test/coverage_baseline.txt' ../AGENTS.md
grep -q 'check_doc_case_counts.sh' ../AGENTS.md
grep -q 'check_contract_inventory.sh' ../AGENTS.md
grep -q 'docs/test-matrix.md' README.md
grep -q 'docs/test-matrix.md' ../README.md
