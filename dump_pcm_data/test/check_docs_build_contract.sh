#!/bin/sh
set -eu

grep -q 'BUILD_SHARED_LIBRARY' Android.mk
grep -q 'dump_control.c' Android.mk
grep -q 'dump_writer.c' Android.mk
grep -q '_POSIX_C_SOURCE=200809L' Android.mk
grep -q '共享库' ../design.md
grep -q 'DUMP_PCM_INTERNAL_DEBUG_API' ../design.md
grep -q '共享库' ../plan.md
