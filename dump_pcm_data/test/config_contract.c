/* Production limits are never instantiated by the PC suite: every test
 * translation unit is compiled with -DDUMP_PCM_PC, so platform_config.h picks
 * the scaled-down platform_config_pc.h (4 sessions / 2 handles / 1 s / 8
 * blocks). Nothing in the suite would notice a change to the values that
 * actually ship.
 *
 * This file is compiled on its own, *without* DUMP_PCM_PC, purely for the
 * assertions below. It defines no symbols, is not linked into the test binary,
 * and must stay out of Android.mk: on device the same header is already in
 * effect, so a compile there would only re-check the same constants.
 *
 * Failure mode is a compile error, not a failing case. */
#include "platform_config.h"

_Static_assert(MAX_SESSIONS == 16, "spec: 16 concurrent sessions");
_Static_assert(MAX_OPEN_FILES == 16, "spec: 16 open file handles");
_Static_assert(IDLE_TIMEOUT_SEC == 5, "spec: 5 s handle idle timeout");
_Static_assert(MAX_QUEUE_BLOCKS == 800, "spec: 800 queued blocks");
_Static_assert(MAX_QUEUE_BYTES == 512UL * 1024UL * 1024UL, "spec: 512 MB queue budget");
_Static_assert(FILE_SIZE_LIMIT == 1024UL * 1024UL * 1024UL, "spec: 1 GB shard limit");
