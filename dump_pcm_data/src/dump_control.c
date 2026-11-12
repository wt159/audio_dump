#include "dump_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
#else
#include <cutils/properties.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

/* Platform default for free-space probing. The injected hook, when set, wins. */
static int default_storage_query(const char *path, uint64_t *available_bytes) {
    if (path == NULL || available_bytes == NULL) {
        return -1;
    }

#if defined(_WIN32)
    {
        ULARGE_INTEGER free_bytes;

        if (!GetDiskFreeSpaceExA(path, &free_bytes, NULL, NULL)) {
            return -1;
        }
        *available_bytes = (uint64_t)free_bytes.QuadPart;
    }
#else
    {
        struct statvfs st;

        if (statvfs(path, &st) != 0) {
            return -1;
        }
        *available_bytes = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    }
#endif

    return 0;
}

static int query_storage(const char *path, uint64_t *available_bytes) {
    dump_storage_query_fn_t fn = g_dump_storage_query_fn;

    return (fn != NULL ? fn : default_storage_query)(path, available_bytes);
}

uint64_t get_timestamp_ms(void) {
    struct timespec ts;

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
    if (g_dump_time_ms_fn != NULL) {
        return g_dump_time_ms_fn();
    }
#endif

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int check_storage(void) {
    uint64_t now = get_timestamp_ms();
    uint64_t last_check = 0;

#ifdef DUMP_PCM_PC
    {
        const char *override_mb = getenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
        if (override_mb != NULL) {
            uint64_t available_mb = strtoull(override_mb, NULL, 10);
            atomic_store(&g_ctx.storage_cache_mb, available_mb);
            atomic_store(&g_ctx.last_storage_check, now);
            if (available_mb < STORAGE_LOW_THRESHOLD_MB) {
                atomic_store(&g_ctx.low_storage_triggered, 1);
                LOGW("Low storage: %" PRIu64 " MB", available_mb);
                return -1;
            }
            return atomic_load(&g_ctx.low_storage_triggered) ? -1 : 0;
        }
    }
#endif

    last_check = atomic_load(&g_ctx.last_storage_check);
    if (now - last_check < STORAGE_CHECK_INTERVAL_MS) {
        return (atomic_load(&g_ctx.storage_cache_mb) >= STORAGE_LOW_THRESHOLD_MB &&
                !atomic_load(&g_ctx.low_storage_triggered)) ? 0 : -1;
    }

    pthread_mutex_lock(&g_storage_mutex);
    last_check = atomic_load(&g_ctx.last_storage_check);
    if (now - last_check < STORAGE_CHECK_INTERVAL_MS) {
        uint64_t cached_mb = atomic_load(&g_ctx.storage_cache_mb);
        pthread_mutex_unlock(&g_storage_mutex);
        return (cached_mb >= STORAGE_LOW_THRESHOLD_MB && !atomic_load(&g_ctx.low_storage_triggered)) ? 0 : -1;
    }

    {
        uint64_t available_bytes = 0;
        uint64_t available_mb = 0;
        const char *dir = file_mgr_get_root_dir();
        if (query_storage(dir, &available_bytes) != 0) {
            pthread_mutex_unlock(&g_storage_mutex);
            LOGE("Failed to get storage info: %s", dir);
            return -1;
        }

        available_mb = available_bytes / (1024ULL * 1024ULL);
        atomic_store(&g_ctx.storage_cache_mb, available_mb);
        atomic_store(&g_ctx.last_storage_check, now);
        LOGI("Storage available: dir=%s available=%" PRIu64 " MB threshold=%d MB", dir, available_mb,
             STORAGE_LOW_THRESHOLD_MB);

        if (available_mb < STORAGE_LOW_THRESHOLD_MB) {
            atomic_store(&g_ctx.low_storage_triggered, 1);
            pthread_mutex_unlock(&g_storage_mutex);
            LOGW("Low storage: %" PRIu64 " MB", available_mb);
            return -1;
        }
    }

    pthread_mutex_unlock(&g_storage_mutex);
    return atomic_load(&g_ctx.low_storage_triggered) ? -1 : 0;
}

int check_enabled(void) {
    int was_enabled = atomic_load(&g_ctx.enabled);
    int override = atomic_load(&g_enabled_override);
    int on = 0;

    if (override >= 0) {
        on = (override == 1);
    } else {
#if defined(DUMP_PCM_PC) || !defined(ANDROID)
        const char *env = getenv(PROPERTY_NAME);
        on = (env != NULL && atoi(env) == 1);
#else
        char value[PROPERTY_VALUE_MAX];
        property_get(PROPERTY_NAME, value, "0");
        on = (strcmp(value, "1") == 0);
#endif
    }

    if (on) {
        atomic_store(&g_ctx.enabled, 1);
        if (!was_enabled) {
            atomic_store(&g_ctx.low_storage_triggered, 0);
            LOGI("Dump enabled");
        }
        return 0;
    }

    atomic_store(&g_ctx.enabled, 0);
    if (was_enabled) {
        LOGI("Dump disabled");
    }
    return -1;
}

int dump_pcm_data_get_status(dump_status_t *status) {
    if (status == NULL || atomic_load(&g_init_state) != DUMP_INIT_READY) {
        return -1;
    }

    pthread_mutex_lock(&g_ctx.mutex);
    status->enabled = atomic_load(&g_ctx.enabled);
    status->low_storage_triggered = atomic_load(&g_ctx.low_storage_triggered);
    status->available_storage_mb = atomic_load(&g_ctx.storage_cache_mb);
    status->queue_pending_blocks = queue_count(&g_ctx.queue);
    status->queue_pending_bytes = queue_bytes(&g_ctx.queue);
    status->active_session_count = count_active_sessions_locked();
    status->dropped_blocks = atomic_load(&g_dropped_blocks);
    pthread_mutex_unlock(&g_ctx.mutex);
    return 0;
}

int dump_pcm_data_get_active_sessions(dump_session_info_t *sessions, int max_count, int *actual_count) {
    if (actual_count == NULL) {
        return -1;
    }
    *actual_count = 0;
    if (max_count <= 0) {
        return 0;
    }
    if (sessions == NULL || atomic_load(&g_init_state) != DUMP_INIT_READY) {
        return -1;
    }

    pthread_mutex_lock(&g_ctx.mutex);
    for (uint32_t i = 0; i < MAX_SESSIONS && *actual_count < max_count; ++i) {
        session_info_t *session = g_ctx.sessions.sessions[i];
        if (session != NULL && session->state == SESSION_ACTIVE) {
            dump_key_copy(sessions[*actual_count].key1, session->key1);
            dump_key_copy(sessions[*actual_count].key2, session->key2);
            sessions[*actual_count].written_bytes = session->file_size;
            sessions[*actual_count].has_format = session->has_format;
            sessions[*actual_count].format = session->format;
            (*actual_count)++;
        }
    }
    pthread_mutex_unlock(&g_ctx.mutex);
    return 0;
}
