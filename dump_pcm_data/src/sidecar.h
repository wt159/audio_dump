#ifndef DUMP_PCM_SIDECAR_H
#define DUMP_PCM_SIDECAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/dump_pcm_data.h"

/* Everything a sidecar needs, copied by value so an open file never depends on
 * the session that created it. */
typedef struct {
    char key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char key2[DUMP_PCM_KEY_MAX_LEN + 1];
    uint32_t generation;
    uint32_t split_index;
    dump_pcm_format_t format;
    uint64_t start_time_unix_ms;
} dump_pcm_file_meta_t;

/* Returns true when s is well-formed UTF-8, which JSON string values require. */
bool sidecar_is_valid_utf8(const char *s);

/* "s16_le" style name, or NULL when the enum is unknown. */
const char *sidecar_format_name(dump_pcm_sample_format_t format);

/* Serialises one sidecar into buf. Returns the length written (excluding the
 * terminator) or -1 when the metadata is invalid or the buffer is too small.
 * When complete is false the end time and total size are omitted. */
int sidecar_build_json(char *buf, size_t size, const char *pcm_file_name,
                       const dump_pcm_file_meta_t *meta, bool complete,
                       uint64_t total_bytes, uint64_t end_time_unix_ms);

#endif /* DUMP_PCM_SIDECAR_H */
