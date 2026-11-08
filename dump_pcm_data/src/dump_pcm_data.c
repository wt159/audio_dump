#include "dump_internal.h"

#include <stdlib.h>

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
#else
#include <cutils/properties.h>
#endif

#define SESSION_ROLLOVER_GAP_MS 2000
#define INIT_RETRY_BACKOFF_MS 500

dump_context_t g_ctx = {0};
pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_init_cond = PTHREAD_COND_INITIALIZER;
_Atomic int g_init_state = DUMP_INIT_UNINITIALIZED;
uint64_t g_last_init_fail_ms = 0;
pthread_mutex_t g_storage_mutex = PTHREAD_MUTEX_INITIALIZER;
dump_statvfs_fn_t g_dump_statvfs_fn = statvfs;
dump_pthread_create_fn_t g_dump_pthread_create_fn = pthread_create;
#if defined(DUMP_PCM_PC) || !defined(ANDROID)
dump_time_ms_fn_t g_dump_time_ms_fn = NULL;
uint64_t g_file_size_limit_for_test = 0;
#endif

uint32_t count_active_sessions_locked(void) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        session_info_t *session = g_ctx.sessions.sessions[i];
        if (session != NULL && session->state == SESSION_ACTIVE) {
            count++;
        }
    }

    return count;
}

static void shutdown_module(void) {
    if (atomic_load(&g_init_state) != DUMP_INIT_READY) {
        return;
    }

    pthread_mutex_lock(&g_ctx.mutex);
    g_ctx.writer_running = 0;
    pthread_cond_broadcast(&g_ctx.cond);
    pthread_mutex_unlock(&g_ctx.mutex);
    pthread_join(g_ctx.writer_thread, NULL);

    pthread_mutex_lock(&g_ctx.mutex);
    queue_destroy(&g_ctx.queue);
    file_mgr_destroy(&g_ctx.file_mgr);
    session_mgr_destroy(&g_ctx.sessions);
    pthread_mutex_unlock(&g_ctx.mutex);

    pthread_cond_destroy(&g_ctx.cond);
    pthread_mutex_destroy(&g_ctx.mutex);
    g_ctx.initialized = 0;
    atomic_store(&g_init_state, DUMP_INIT_UNINITIALIZED);
}

static int do_init_once(void) {
    pthread_condattr_t cond_attr;
    clockid_t cond_clock = CLOCK_REALTIME;

    logger_init(NULL);
    LOGI("Initializing dump module");

    if (pthread_mutex_init(&g_ctx.mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_condattr_init(&cond_attr) != 0) {
        pthread_mutex_destroy(&g_ctx.mutex);
        return -1;
    }
    /* winpthreads and some libc builds reject CLOCK_MONOTONIC here. Losing the
     * monotonic clock only weakens timeout accuracy; it must not disable dumping.
     * The writer thread reads g_ctx.cond_clock_id so both sides always agree. */
    if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0) {
        cond_clock = CLOCK_MONOTONIC;
    } else {
        LOGW("Condition variable clock: CLOCK_MONOTONIC unsupported, falling back to CLOCK_REALTIME");
    }
    if (pthread_cond_init(&g_ctx.cond, &cond_attr) != 0) {
        pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&g_ctx.mutex);
        return -1;
    }
    pthread_condattr_destroy(&cond_attr);
    g_ctx.cond_clock_id = cond_clock;

    queue_init(&g_ctx.queue, MAX_QUEUE_BLOCKS, MAX_QUEUE_BYTES);
    session_mgr_init(&g_ctx.sessions);
    if (file_mgr_init(&g_ctx.file_mgr) != 0) {
        pthread_cond_destroy(&g_ctx.cond);
        pthread_mutex_destroy(&g_ctx.mutex);
        return -1;
    }

    g_ctx.initialized = 0;
    g_ctx.writer_running = 1;
    g_ctx.next_generation = 1;
    atomic_store(&g_ctx.low_storage_triggered, 0);
    atomic_store(&g_ctx.last_storage_check, 0);
    atomic_store(&g_ctx.storage_cache_mb, 0);
    /* g_ctx.enabled is owned by check_enabled(), which runs before init now.
     * Resetting it here would drop the very first enabled write. */

    file_mgr_cleanup(100, 7);
    if (g_dump_pthread_create_fn(&g_ctx.writer_thread, NULL, writer_thread_func, NULL) != 0) {
        g_ctx.writer_running = 0;
        file_mgr_destroy(&g_ctx.file_mgr);
        pthread_cond_destroy(&g_ctx.cond);
        pthread_mutex_destroy(&g_ctx.mutex);
        return -1;
    }

    g_ctx.initialized = 1;
    atexit(shutdown_module);
    LOGI("Dump module initialized");
    return 0;
}

static int ensure_initialized(void) {
    if (atomic_load(&g_init_state) == DUMP_INIT_READY) {
        return 0;
    }

    pthread_mutex_lock(&g_init_mutex);
    for (;;) {
        int state = atomic_load(&g_init_state);
        if (state == DUMP_INIT_READY) {
            pthread_mutex_unlock(&g_init_mutex);
            return 0;
        }
        if (state == DUMP_INIT_INITIALIZING) {
            pthread_cond_wait(&g_init_cond, &g_init_mutex);
            continue;
        }
        if (state == DUMP_INIT_FAILED && get_timestamp_ms() - g_last_init_fail_ms < INIT_RETRY_BACKOFF_MS) {
            pthread_mutex_unlock(&g_init_mutex);
            return -1;
        }

        atomic_store(&g_init_state, DUMP_INIT_INITIALIZING);
        pthread_mutex_unlock(&g_init_mutex);

        if (do_init_once() == 0) {
            pthread_mutex_lock(&g_init_mutex);
            atomic_store(&g_init_state, DUMP_INIT_READY);
            pthread_cond_broadcast(&g_init_cond);
            pthread_mutex_unlock(&g_init_mutex);
            return 0;
        }

        pthread_mutex_lock(&g_init_mutex);
        g_last_init_fail_ms = get_timestamp_ms();
        atomic_store(&g_init_state, DUMP_INIT_FAILED);
        pthread_cond_broadcast(&g_init_cond);
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }
}

void dump_pcm_data_write(const char *buffer, uint32_t buffer_size, const char *key1, const char *key2) {
    data_block_t block = {0};
    session_info_t *session = NULL;
    session_info_t *rollover_session = NULL;
    session_info_t *stopped_session = NULL;
    uint64_t now = 0;

    if (buffer_size == 0 || (buffer == NULL && buffer_size > 0) ||
        !dump_key_is_valid(key1) || !dump_key_is_valid(key2)) {
        return;
    }
    /* The master switch gates initialization: while it is off the module must
     * not create the writer thread, create directories, or run the retention
     * cleanup. check_enabled() only touches zero-initialized atomics, so it is
     * safe to call before ensure_initialized(). */
    if (check_enabled() != 0) {
        return;
    }
    if (ensure_initialized() != 0) {
        return;
    }
    now = get_timestamp_ms();
    if (check_storage() != 0) {
        return;
    }

    pthread_mutex_lock(&g_ctx.mutex);
    if (!atomic_load(&g_ctx.enabled) || atomic_load(&g_ctx.low_storage_triggered) || queue_is_full(&g_ctx.queue)) {
        pthread_mutex_unlock(&g_ctx.mutex);
        return;
    }

    session = session_find_accepting(&g_ctx.sessions, key1, key2);
    if (session != NULL && now > session->last_write_time &&
        now - session->last_write_time > SESSION_ROLLOVER_GAP_MS) {
        rollover_session = session_create(&g_ctx.sessions, key1, key2, g_ctx.next_generation++);
        if (rollover_session != NULL) {
            stopped_session = session;
            session = rollover_session;
        }
    }
    if (session == NULL) {
        if (check_storage() != 0 || session_live_count(&g_ctx.sessions) >= MAX_SESSIONS) {
            LOGW("Drop PCM block: no session available for key1=%s key2=%s", key1, key2);
            pthread_mutex_unlock(&g_ctx.mutex);
            return;
        }

        session = session_create(&g_ctx.sessions, key1, key2, g_ctx.next_generation++);
        if (session == NULL) {
            LOGE("Failed to create dump session: key1=%s key2=%s", key1, key2);
            pthread_mutex_unlock(&g_ctx.mutex);
            return;
        }
        LOGI("Start dump session: key1=%s key2=%s gen=%u", key1, key2, session->generation);
    }

    session->last_write_time = now;

    dump_key_copy(block.key1, key1);
    dump_key_copy(block.key2, key2);
    block.generation = session->generation;
    block.buffer = (char *)buffer;
    block.buffer_size = buffer_size;
    // Call-entry time; do not use writer-thread timing here.
    block.enqueue_time = now;
    if (queue_push(&g_ctx.queue, &block) != 0) {
        LOGW("Drop PCM block: queue full key1=%s key2=%s size=%u", key1, key2, buffer_size);
        if (stopped_session != NULL) {
            session_destroy(&g_ctx.sessions, session);
            session = stopped_session;
        }
        pthread_mutex_unlock(&g_ctx.mutex);
        return;
    }

    session->last_write_time = now;
    session->pending_blocks++;
    if (stopped_session != NULL) {
        stop_session_locked(stopped_session);
    }
    pthread_cond_signal(&g_ctx.cond);
    pthread_mutex_unlock(&g_ctx.mutex);
}

void dump_pcm_data_stop(const char *key1, const char *key2) {
    session_info_t *session = NULL;

    if (atomic_load(&g_init_state) != DUMP_INIT_READY || !dump_key_is_valid(key1) || !dump_key_is_valid(key2)) {
        return;
    }

    pthread_mutex_lock(&g_ctx.mutex);
    session = session_find_accepting(&g_ctx.sessions, key1, key2);
    if (session != NULL) {
        LOGI("Stop dump session: key1=%s key2=%s", key1, key2);
        stop_session_locked(session);
    }
    pthread_mutex_unlock(&g_ctx.mutex);
}

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
void dump_pcm_set_statvfs_fn_for_test(dump_statvfs_fn_t fn) {
    g_dump_statvfs_fn = (fn != NULL) ? fn : statvfs;
}

void dump_pcm_set_pthread_create_fn_for_test(dump_pthread_create_fn_t fn) {
    g_dump_pthread_create_fn = (fn != NULL) ? fn : pthread_create;
}

void dump_pcm_set_time_ms_fn_for_test(dump_time_ms_fn_t fn) {
    g_dump_time_ms_fn = fn;
}

void dump_pcm_set_file_size_limit_for_test(uint64_t bytes) {
    g_file_size_limit_for_test = bytes;
}
#endif