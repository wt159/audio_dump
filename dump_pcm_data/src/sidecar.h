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

/* Process-wide switch, default off. Returns the resulting state. */
bool sidecar_set_enabled(int enabled);
bool sidecar_is_enabled(void);

/* CLOCK_REALTIME in milliseconds, or 0 when the clock is unavailable. */
uint64_t sidecar_now_unix_ms(void);

/* Writes json to <pcm_path with its extension replaced by .json>. The payload
 * is staged in <name>.json.tmp and renamed into place so a reader never sees a
 * partial file. Returns 0 on success, -1 on any failure (the temporary file is
 * removed and an existing .json is left untouched). */
int sidecar_write_file(const char *pcm_path, const char *json, size_t json_len);

/* Builds and writes the sidecar for pcm_path. Returns -1 when the metadata is
 * invalid or the file cannot be written. The enabled switch is policy and is
 * checked by the caller (file_mgr), so a file that opened with a sidecar can
 * still be finalised after the switch is turned off. */
int sidecar_write_meta(const char *pcm_path, const dump_pcm_file_meta_t *meta,
                       bool complete, uint64_t total_bytes,
                       uint64_t end_time_unix_ms);

#endif /* DUMP_PCM_SIDECAR_H */
