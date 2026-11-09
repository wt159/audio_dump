#ifndef DUMP_PCM_DATA_H
#define DUMP_PCM_DATA_H

#include <stdbool.h>
#include <stdint.h>

#define DUMP_PCM_KEY_MAX_LEN 64

#if defined(__GNUC__)
#define DUMP_PCM_API __attribute__((visibility("default")))
#else
#define DUMP_PCM_API
#endif

DUMP_PCM_API void dump_pcm_data_write(const char *buffer, uint32_t buffer_size, const char *key1, const char *key2);
DUMP_PCM_API void dump_pcm_data_stop(const char *key1, const char *key2);

/* Runtime master switch. enabled: 1 on, 0 off, -1 fall back to the platform
 * source (Android property, desktop environment variable). Takes effect on the
 * next write; turning it from off to on also clears the low-storage latch. */
DUMP_PCM_API void dump_pcm_set_enabled(int enabled);

/* Overrides the dump root directory. Must be called before the module is
 * initialized; returns -1 and leaves the directory unchanged otherwise. */
DUMP_PCM_API int dump_pcm_set_root_dir(const char *dir);

/* Enables the JSON metadata sidecar next to each .pcm file. The switch can be
 * flipped at runtime and only affects files opened after the call. Off by
 * default. Returns 0 on success. */
DUMP_PCM_API int dump_pcm_set_metadata_enabled(int enabled);

typedef struct {
    bool enabled;
    bool low_storage_triggered;
    uint64_t available_storage_mb;
    uint32_t queue_pending_blocks;
    uint64_t queue_pending_bytes;
    uint32_t active_session_count;
} dump_status_t;

typedef enum {
    DUMP_PCM_FORMAT_UNKNOWN = 0,
    DUMP_PCM_FORMAT_S16_LE,
    DUMP_PCM_FORMAT_S24_LE,
    DUMP_PCM_FORMAT_S24_3LE,
    DUMP_PCM_FORMAT_S32_LE,
    DUMP_PCM_FORMAT_FLOAT_LE
} dump_pcm_sample_format_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channel_count;
    uint16_t bits_per_sample;
    dump_pcm_sample_format_t sample_format;
} dump_pcm_format_t;

/* Same contract as dump_pcm_data_write(), plus the stream format, which is
 * captured once on the first block of a session and ignored afterwards.
 * Passing NULL is exactly equivalent to the legacy call. */
DUMP_PCM_API void dump_pcm_data_write_ex(const char *buffer, uint32_t buffer_size,
                                         const char *key1, const char *key2,
                                         const dump_pcm_format_t *format);

typedef struct {
    char key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char key2[DUMP_PCM_KEY_MAX_LEN + 1];
    uint64_t written_bytes;
    bool has_format;
    dump_pcm_format_t format;
} dump_session_info_t;

/* Query APIs return -1 when the module has never been enabled. The master
 * switch gates initialization, so "disabled" and "not initialized" are
 * intentionally indistinguishable to callers. Desktop builds opt in with
 * DUMP_PCM_DESKTOP_OBSERVE, which also exports the symbols. */
#if defined(DUMP_PCM_DESKTOP_OBSERVE)
#define DUMP_PCM_OBSERVE_API DUMP_PCM_API
#else
#define DUMP_PCM_OBSERVE_API
#endif

#if defined(DUMP_PCM_INTERNAL_DEBUG_API) || defined(DUMP_PCM_DESKTOP_OBSERVE)
DUMP_PCM_OBSERVE_API int dump_pcm_data_get_status(dump_status_t *status);
DUMP_PCM_OBSERVE_API int dump_pcm_data_get_active_sessions(dump_session_info_t *sessions, int max_count,
                                                           int *actual_count);
#endif

#endif
