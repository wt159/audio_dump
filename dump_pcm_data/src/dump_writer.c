#include "dump_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "file_mgr_job.h"
#include "sidecar.h"

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

/* Close a handle that was detached from its session while holding C.
 *
 * MUST be called with NO module lock held (L4). capture_close() takes F for a
 * pure-memory slot removal, and run_jobs() performs fclose + the final sidecar
 * outside every lock. A handle that is no longer tracked yields zero jobs, so
 * a concurrent close is harmless. */
void run_deferred_close(FILE *fp) {
    fm_job_t jobs[FM_JOB_MAX_PER_STEP];
    size_t count = 0;

    if (fp == NULL) {
        return;
    }
    count = file_mgr_capture_close(&g_ctx.file_mgr, fp, jobs);
    file_mgr_run_jobs(&g_ctx.file_mgr, jobs, count);
}

FILE *close_session_file_locked(session_info_t *session) {
    FILE *fp = NULL;

    if (session == NULL) {
        return NULL;
    }
    fp = session->fp;
    session->fp = NULL;
    return fp;
}

FILE *destroy_session_locked(session_info_t *session) {
    FILE *fp = NULL;

    if (session == NULL) {
        return NULL;
    }

    fp = close_session_file_locked(session);
    session_destroy(&g_ctx.sessions, session);
    /* Counted here, not in the writer loop: a stop with nothing pending destroys
     * the session on the caller thread, and those must count too. */
    atomic_fetch_add(&g_completed_sessions, 1);
    return fp;
}

FILE *stop_session_locked(session_info_t *session) {
    if (session == NULL) {
        return NULL;
    }

    session->state = SESSION_STOPPING;
    session->accepting = false;
    if (session->pending_blocks == 0) {
        return destroy_session_locked(session);
    }
    LOGI("Session stopping: key1=%s key2=%s", session->key1, session->key2);
    return NULL;
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

/* The sweep is already outside C; using capture/run directly keeps the
 * production path off the test-only compatibility layer. */
static int run_retention_sweep(void) {
    fm_job_t job;

    file_mgr_capture_cleanup(&g_ctx.file_mgr, 100, 7, &job);
    return file_mgr_run_job(&g_ctx.file_mgr, &job);
}

static void maybe_run_retention_cleanup(void) {
    if (atomic_load(&g_completed_sessions) < RETENTION_CLEANUP_SESSION_INTERVAL) {
        return;
    }
    atomic_store(&g_completed_sessions, 0);
    if (run_retention_sweep() != 0) {
        LOGW("Periodic retention cleanup failed");
    }
}

/* POD snapshot taken under C. fp_tag is an IDENTITY LABEL only: the publish
 * phase compares it against the live session, and no I/O ever targets it. */
static void snapshot_session(const session_info_t *session, session_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));
    dump_key_copy(snap->key1, session->key1);
    dump_key_copy(snap->key2, session->key2);
    snap->generation = session->generation;
    snap->split_index = session->split_index;
    snprintf(snap->file_path, sizeof(snap->file_path), "%s", session->file_path);
    snap->file_size = session->file_size;
    snap->file_start_unix_ms = session->file_start_unix_ms;
    snap->has_format = session->has_format;
    snap->format = session->format;
    snap->fp_tag = session->fp;
    snap->pending_blocks = session->pending_blocks;
    snap->state = (int)session->state;
    /* generate_filename() must see the same buffer capacity the legacy
     * get_handle() call did, so long-name clipping stays byte-for-byte equal. */
    snap->path_capacity = sizeof(snap->file_path);
}

typedef struct {
    bool valid;                 /* a REOPEN happened; publish these fields */
    FILE *fp;                   /* NULL when the open job failed */
    char path[256];
    uint64_t file_size;
    uint64_t start_unix_ms;
    uint32_t split_index;
} publish_result_t;

/* EXECUTION PHASE. Called with NO module lock held. Every capture_* takes F
 * and releases it before returning; every run_* performs its blocking I/O
 * lock-free. The last F unlock in this function therefore happens BEFORE the
 * publish phase takes C, which is exactly invariant L2. */
static void execute_block(session_snapshot_t snap, const char *payload,
                          size_t payload_size, publish_result_t *pub, int *write_ok) {
    session_info_t view = {0};
    session_file_action_t action;
    FILE *write_fp = NULL;
    bool tracked = false;

    memset(pub, 0, sizeof(*pub));
    *write_ok = -1;

    /* session_file_action() is pure and stays unit-testable; the tracked flag
     * now comes from the F-owned slot table instead of a C-held lookup. */
    view.fp = snap.fp_tag;
    view.file_size = snap.file_size;
    tracked = (snap.fp_tag != NULL) &&
              file_mgr_slot_tracked(&g_ctx.file_mgr, snap.fp_tag);
    action = session_file_action(&view, tracked, get_file_size_limit_bytes());

    if (action == SESSION_FILE_ROLLOVER) {
        run_deferred_close(snap.fp_tag);
        snap.split_index++;
        snap.file_path[0] = '\0';
        snap.file_size = 0;
        snap.file_start_unix_ms = 0;
        /* fp_tag deliberately stays at its decision-time value. The publish
         * phase uses it to detect a concurrent stop/reopen that swapped the
         * session's handle in the meantime. */
        action = SESSION_FILE_REOPEN;
    }

    if (action == SESSION_FILE_REOPEN) {
        fm_job_t jobs[FM_JOB_MAX_PER_STEP];
        size_t count = 0;
        size_t i = 0;
        FILE *new_fp = NULL;
        bool is_new = false;
        char new_path[256] = {0};
        uint64_t start = snap.file_start_unix_ms;

        file_mgr_capture_open(&g_ctx.file_mgr, &snap, jobs, &count);
        file_mgr_run_jobs(&g_ctx.file_mgr, jobs, count);
        for (i = 0; i < count; ++i) {
            if (jobs[i].kind == FM_JOB_OPEN) {
                new_fp = jobs[i].fp;
                is_new = jobs[i].is_new;
                snprintf(new_path, sizeof(new_path), "%s", jobs[i].path);
            }
        }

        pub->valid = true;
        pub->fp = new_fp;
        snprintf(pub->path, sizeof(pub->path), "%s", new_path);
        pub->split_index = snap.split_index;
        pub->file_size = is_new ? 0 : snap.file_size;
        if (is_new || start == 0) {
            start = sidecar_now_unix_ms();
        }
        pub->start_unix_ms = start;

        if (new_fp != NULL) {
            /* The legacy path has no format, so it must produce no sidecar. */
            if (snap.has_format) {
                dump_pcm_file_meta_t meta = {0};
                fm_job_t initial;

                dump_key_copy(meta.key1, snap.key1);
                dump_key_copy(meta.key2, snap.key2);
                meta.generation = snap.generation;
                meta.split_index = snap.split_index;
                meta.format = snap.format;
                meta.start_time_unix_ms = start;
                file_mgr_capture_initial_meta(&g_ctx.file_mgr, new_fp, &meta,
                                              is_new, &initial);
                file_mgr_run_job(&g_ctx.file_mgr, &initial);
            }
            write_fp = new_fp;
        }
    } else {
        /* SESSION_FILE_USE: capture_write resolves the target from the slot
         * table under F, so the pointer that actually reaches fwrite is not a
         * value carried across the C unlock window. */
        write_fp = snap.fp_tag;
    }

    if (write_fp != NULL) {
        fm_job_t write_job;

        file_mgr_capture_write(&g_ctx.file_mgr, write_fp, payload, payload_size,
                               &write_job);
        if (write_job.kind != FM_JOB_NONE) {
            *write_ok = file_mgr_run_job(&g_ctx.file_mgr, &write_job);
        }
    }
}

void *writer_thread_func(void *arg) {
    (void)arg;

    /* Retention scan is opendir + per-file stat + O(n^2) sort + unlink, so it
     * must not run on the caller thread. file_mgr_init() already created the
     * root directory, which check_storage() needs, so only the cleanup moves. */
    if (run_retention_sweep() != 0) {
        LOGW("Initial retention cleanup failed");
    }

    while (g_ctx.writer_running) {
        data_block_t block = {0};
        session_snapshot_t snap = {0};
        publish_result_t pub;
        session_info_t *session = NULL;
        FILE *deferred[2] = {NULL, NULL};
        size_t deferred_count = 0;
        size_t i = 0;
        bool have_session = false;
        int write_ok = -1;

        /* ---- DECISION PHASE: C-held, pure memory, zero file I/O ---------- */
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

        session = session_find_by_generation(&g_ctx.sessions, block.key1, block.key2,
                                             block.generation);
        if (session != NULL) {
            snapshot_session(session, &snap);
            have_session = true;
        }
        pthread_mutex_unlock(&g_ctx.mutex);

        /* ---- EXECUTION PHASE: no C, all blocking I/O happens here -------- */
        if (have_session) {
            execute_block(snap, block.buffer, block.buffer_size, &pub, &write_ok);
        } else {
            memset(&pub, 0, sizeof(pub));
        }

        /* ---- PUBLISH PHASE: retake C. L2 guarantees F is already free. --- */
        pthread_mutex_lock(&g_ctx.mutex);
        session = session_find_by_generation(&g_ctx.sessions, block.key1, block.key2,
                                             block.generation);
        if (session == NULL) {
            /* The session vanished (or was destroyed by a concurrent stop).
             * Any handle opened in the execution phase must be closed, but the
             * I/O has to wait until C is released. */
            if (pub.valid && pub.fp != NULL) {
                deferred[deferred_count++] = pub.fp;
            }
        } else {
            bool identity_ok = (session->fp == snap.fp_tag);

            if (have_session && pub.valid) {
                if (identity_ok) {
                    session->fp = pub.fp;
                    snprintf(session->file_path, sizeof(session->file_path), "%s", pub.path);
                    session->split_index = pub.split_index;
                    session->file_size = pub.file_size;
                    session->file_start_unix_ms = pub.start_unix_ms;
                } else if (pub.fp != NULL) {
                    /* A concurrent reopen/stop changed the handle; discard the
                     * one we just opened instead of publishing a stale pointer. */
                    deferred[deferred_count++] = pub.fp;
                }
            }
            if (write_ok == 0 && identity_ok) {
                session->file_size += block.buffer_size;
            }
            if (session->pending_blocks > 0) {
                session->pending_blocks--;
            }
            if (session->state == SESSION_STOPPING && session->pending_blocks == 0) {
                FILE *closed = destroy_session_locked(session);

                if (closed != NULL && deferred_count < 2) {
                    deferred[deferred_count++] = closed;
                }
            }
        }
        pthread_mutex_unlock(&g_ctx.mutex);

        /* ---- DEFERRED I/O: outside C (L4) -------------------------------- */
        for (i = 0; i < deferred_count; ++i) {
            run_deferred_close(deferred[i]);
        }

        maybe_run_retention_cleanup();
        free(block.buffer);
    }

    return NULL;
}
