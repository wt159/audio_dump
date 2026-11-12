#ifndef DUMP_PCM_INTERNAL_H
#define DUMP_PCM_INTERNAL_H

#include "../include/dump_pcm_data.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define STORAGE_CHECK_INTERVAL_MS 1000
#define STORAGE_LOW_THRESHOLD_MB 500

#include "dump_test_hooks.h"
#include "file_mgr.h"
#include "key_utils.h"
#include "logger.h"
#include "queue.h"
#include "session.h"

/* Invariants:
 * - pending_blocks increments only after queue_push() succeeds.
 * - SESSION_STOPPING sessions do not accept new blocks.
 * - queued blocks keep generation identity until writer-side lookup.
 * - drained stop may finalize on the caller thread; normal queued writes
 *   finalize on the writer thread.
 */

typedef enum {
    DUMP_INIT_UNINITIALIZED = 0,
    DUMP_INIT_INITIALIZING,
    DUMP_INIT_READY,
    DUMP_INIT_FAILED
} dump_init_state_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    /* Clock the condition variable was created with. Every timed wait must
     * compute its deadline from this same clock id. */
    clockid_t cond_clock_id;
    pthread_t writer_thread;
    queue_t queue;
    session_manager_t sessions;
    file_manager_t file_mgr;
    int initialized;
    int writer_running;
    atomic_int low_storage_triggered;
    atomic_uint_fast64_t last_storage_check;
    atomic_uint_fast64_t storage_cache_mb;
    atomic_int enabled;
    uint32_t next_generation;
} dump_context_t;

extern dump_context_t g_ctx;
extern pthread_mutex_t g_init_mutex;
extern pthread_cond_t g_init_cond;
extern _Atomic int g_init_state;
extern atomic_int g_enabled_override;
extern atomic_ullong g_dropped_blocks;
extern uint64_t g_last_init_fail_ms;
extern pthread_mutex_t g_storage_mutex;
extern dump_storage_query_fn_t g_dump_storage_query_fn;
extern dump_pthread_create_fn_t g_dump_pthread_create_fn;

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
extern dump_time_ms_fn_t g_dump_time_ms_fn;
extern uint64_t g_file_size_limit_for_test;
#endif

uint64_t get_timestamp_ms(void);
int check_storage(void);
int check_enabled(void);
void close_session_file_locked(session_info_t *session);
void destroy_session_locked(session_info_t *session);
void stop_session_locked(session_info_t *session);
uint64_t get_file_size_limit_bytes(void);
uint32_t count_active_sessions_locked(void);
void *writer_thread_func(void *arg);

#endif
