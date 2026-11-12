#include "dump_internal.h"

#include <stdatomic.h>
#include <stdlib.h>

#define WRITER_POLL_INTERVAL_MS 100

/* Incremented from whichever thread finishes a session; the sweep itself is
 * done by the writer thread, which is the only thread allowed to do it. */
static atomic_uint g_completed_sessions = 0;


uint64_t get_file_size_limit_bytes(void) {
#if defined(DUMP_PCM_PC) || !defined(ANDROID)
    if (g_file_size_limit_for_test > 0) {
        return g_file_size_limit_for_test;
    }
#endif

    return FILE_SIZE_LIMIT;
}

void close_session_file_locked(session_info_t *session) {
    if (session->fp != NULL && file_mgr_is_tracking(&g_ctx.file_mgr, session->fp)) {
        file_mgr_close(&g_ctx.file_mgr, session->fp);
    }
    session->fp = NULL;
}

void destroy_session_locked(session_info_t *session) {
    if (session == NULL) {
        return;
    }

    close_session_file_locked(session);
    session_destroy(&g_ctx.sessions, session);
    /* Counted here, not in the writer loop: a stop with nothing pending destroys
     * the session on the caller thread, and those must count too. */
    atomic_fetch_add(&g_completed_sessions, 1);
}

void stop_session_locked(session_info_t *session) {
    if (session == NULL) {
        return;
    }

    session->state = SESSION_STOPPING;
    session->accepting = false;
    if (session->pending_blocks == 0) {
        destroy_session_locked(session);
    } else {
        LOGI("Session stopping: key1=%s key2=%s", session->key1, session->key2);
    }
}

/* How many completed sessions may pass before the retention sweep runs again.
 *
 * Running it only when the writer thread starts bounds the directory for a
 * process that restarts, and does nothing for one that does not: an audio HAL
 * lives for days, and files that cross the seven-day line during its lifetime
 * would never be pruned. The sweep is an opendir plus a stat per .pcm file, so
 * this interval keeps the cost off the caller thread and out of the hot path
 * while still re-applying the bound well before the directory matters. */
#define RETENTION_CLEANUP_SESSION_INTERVAL 8

static void maybe_run_retention_cleanup(void) {
    if (atomic_load(&g_completed_sessions) < RETENTION_CLEANUP_SESSION_INTERVAL) {
        return;
    }
    atomic_store(&g_completed_sessions, 0);
    if (file_mgr_cleanup(100, 7) != 0) {
        LOGW("Periodic retention cleanup failed");
    }
}

void *writer_thread_func(void *arg) {
    (void)arg;

    /* Retention scan is opendir + per-file stat + O(n^2) sort + unlink, so it
     * must not run on the caller thread. file_mgr_init() already created the
     * root directory, which check_storage() needs, so only the cleanup moves. */
    if (file_mgr_cleanup(100, 7) != 0) {
        LOGW("Initial retention cleanup failed");
    }

    while (g_ctx.writer_running) {
        data_block_t block = {0};
        session_info_t *session = NULL;
        FILE *fp = NULL;
        int write_ok = -1;

        pthread_mutex_lock(&g_ctx.mutex);
        while (queue_is_empty(&g_ctx.queue) && g_ctx.writer_running) {
            struct timespec ts;
            /* Must match the clock the condition variable was created with. */
            clock_gettime(g_ctx.cond_clock_id, &ts);
            ts.tv_nsec += WRITER_POLL_INTERVAL_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_ctx.cond, &g_ctx.mutex, &ts);
        }

        if (!g_ctx.writer_running) {
            pthread_mutex_unlock(&g_ctx.mutex);
            break;
        }

        if (queue_pop(&g_ctx.queue, &block) != 0) {
            pthread_mutex_unlock(&g_ctx.mutex);
            /* The queue going idle is the cheapest moment to re-apply the
             * retention bound; the sweep must not hold the mutex. */
            maybe_run_retention_cleanup();
            continue;
        }

        session = session_find_by_generation(&g_ctx.sessions, block.key1, block.key2, block.generation);
        if (session != NULL) {
            bool is_new = false;
            bool tracked = session->fp != NULL && file_mgr_is_tracking(&g_ctx.file_mgr, session->fp);
            session_file_action_t action = session_file_action(session, tracked, get_file_size_limit_bytes());

            if (action == SESSION_FILE_ROLLOVER) {
                if (tracked) {
                    file_mgr_close(&g_ctx.file_mgr, session->fp);
                }
                session->split_index++;
                session->fp = NULL;
                session->file_path[0] = '\0';
                session->file_size = 0;
                session->file_start_unix_ms = 0;
                action = SESSION_FILE_REOPEN;
            }

            if (action == SESSION_FILE_REOPEN) {
                session->fp = file_mgr_get_handle(&g_ctx.file_mgr, session->key1, session->key2,
                                                  session->generation, session->split_index,
                                                  session->file_path,
                                                  sizeof(session->file_path), &is_new);
                if (session->fp != NULL) {
                    if (is_new) {
                        session->file_size = 0;
                        session->file_start_unix_ms = sidecar_now_unix_ms();
                    } else if (session->file_start_unix_ms == 0) {
                        session->file_start_unix_ms = sidecar_now_unix_ms();
                    }
                    /* The legacy path has no format, so it must produce no
                     * sidecar at all; file_mgr_attach_meta() is the only
                     * writer-side hook. */
                    if (session->has_format) {
                        dump_pcm_file_meta_t meta = {0};

                        dump_key_copy(meta.key1, session->key1);
                        dump_key_copy(meta.key2, session->key2);
                        meta.generation = session->generation;
                        meta.split_index = session->split_index;
                        meta.format = session->format;
                        meta.start_time_unix_ms = session->file_start_unix_ms;
                        file_mgr_attach_meta(&g_ctx.file_mgr, session->fp, &meta, is_new);
                    }
                }
            }
            fp = session->fp;
        }
        pthread_mutex_unlock(&g_ctx.mutex);

        if (fp != NULL) {
            write_ok = file_mgr_write(&g_ctx.file_mgr, fp, block.buffer, block.buffer_size);
        }

        pthread_mutex_lock(&g_ctx.mutex);
        session = session_find_by_generation(&g_ctx.sessions, block.key1, block.key2, block.generation);
        if (session != NULL) {
            if (write_ok == 0) {
                session->file_size += block.buffer_size;
            }
            if (session->pending_blocks > 0) {
                session->pending_blocks--;
            }
            if (session->state == SESSION_STOPPING && session->pending_blocks == 0) {
                destroy_session_locked(session);
            }
        }
        pthread_mutex_unlock(&g_ctx.mutex);

        maybe_run_retention_cleanup();
        free(block.buffer);
    }

    return NULL;
}
