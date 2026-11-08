#ifndef DUMP_PCM_DATA_H
#define DUMP_PCM_DATA_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/statvfs.h>

#define DUMP_PCM_KEY_MAX_LEN 64

#if defined(__GNUC__)
#define DUMP_PCM_API __attribute__((visibility("default")))
#else
#define DUMP_PCM_API
#endif

DUMP_PCM_API void dump_pcm_data_write(const char *buffer, uint32_t buffer_size, const char *key1, const char *key2);
DUMP_PCM_API void dump_pcm_data_stop(const char *key1, const char *key2);

typedef struct {
    bool enabled;
    bool low_storage_triggered;
    uint64_t available_storage_mb;
    uint32_t queue_pending_blocks;
    uint64_t queue_pending_bytes;
    uint32_t active_session_count;
} dump_status_t;

typedef struct {
    char key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char key2[DUMP_PCM_KEY_MAX_LEN + 1];
    uint64_t written_bytes;
} dump_session_info_t;

/* Query APIs return -1 when the module has never been enabled. The master
 * switch gates initialization, so "disabled" and "not initialized" are
 * intentionally indistinguishable to callers. */
#ifdef DUMP_PCM_INTERNAL_DEBUG_API
int dump_pcm_data_get_status(dump_status_t *status);
int dump_pcm_data_get_active_sessions(dump_session_info_t *sessions, int max_count, int *actual_count);
#endif

typedef int (*dump_statvfs_fn_t)(const char *path, struct statvfs *buf);
typedef int (*dump_pthread_create_fn_t)(pthread_t *thread, const pthread_attr_t *attr,
                                        void *(*start_routine)(void *), void *arg);
typedef uint64_t (*dump_time_ms_fn_t)(void);

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
void dump_pcm_set_statvfs_fn_for_test(dump_statvfs_fn_t fn);
void dump_pcm_set_pthread_create_fn_for_test(dump_pthread_create_fn_t fn);
void dump_pcm_set_time_ms_fn_for_test(dump_time_ms_fn_t fn);
#endif

#endif
