#!/bin/sh
set -eu

grep -q '^check-contracts: check-verification-flow check-header-contract check-docs-build-contract check-shared-library-exports$' Makefile
grep -q '^run: check-contracts $(TARGET)$' Makefile
grep -q '^coverage: check-contracts all$' Makefile
