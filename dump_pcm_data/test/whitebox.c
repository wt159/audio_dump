#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define DUMP_PCM_INTERNAL_DEBUG_API 1

#include "../include/dump_pcm_data.h"
#include "../src/file_mgr.h"
#include "../src/key_utils.h"
#include "../src/logger.h"
#include "../src/queue.h"
#include "../src/session.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define file_mgr_get_root_dir file_mgr_get_root_dir__whitebox
#define file_mgr_set_root_dir file_mgr_set_root_dir__whitebox
#define file_mgr_init file_mgr_init__whitebox
#define file_mgr_destroy file_mgr_destroy__whitebox
#define file_mgr_get_handle file_mgr_get_handle__whitebox
#define file_mgr_write file_mgr_write__whitebox
#define file_mgr_close file_mgr_close__whitebox
#define file_mgr_is_tracking file_mgr_is_tracking__whitebox
#define file_mgr_cleanup file_mgr_cleanup__whitebox
#define file_mgr_set_fwrite_fn_for_test file_mgr_set_fwrite_fn_for_test__whitebox
#define file_mgr_set_fflush_fn_for_test file_mgr_set_fflush_fn_for_test__whitebox
#define file_mgr_set_timestamp_override_for_test file_mgr_set_timestamp_override_for_test__whitebox
#include "../src/file_mgr.c"

#define dump_pcm_data_write dump_pcm_data_write__whitebox
#define dump_pcm_data_stop dump_pcm_data_stop__whitebox
#define dump_pcm_data_get_status dump_pcm_data_get_status__whitebox
#define dump_pcm_data_get_active_sessions dump_pcm_data_get_active_sessions__whitebox
#define dump_pcm_set_time_ms_fn_for_test dump_pcm_set_time_ms_fn_for_test__whitebox
#define dump_pcm_set_statvfs_fn_for_test dump_pcm_set_statvfs_fn_for_test__whitebox
#define dump_pcm_set_pthread_create_fn_for_test dump_pcm_set_pthread_create_fn_for_test__whitebox
#define dump_pcm_set_file_size_limit_for_test dump_pcm_set_file_size_limit_for_test__whitebox

/* mangle globals and internal functions to avoid link conflicts */
#define g_ctx g_ctx__whitebox
#define g_init_mutex g_init_mutex__whitebox
#define g_init_cond g_init_cond__whitebox
#define g_init_state g_init_state__whitebox
#define g_last_init_fail_ms g_last_init_fail_ms__whitebox
#define g_storage_mutex g_storage_mutex__whitebox
#define g_dump_statvfs_fn g_dump_statvfs_fn__whitebox
#define g_dump_pthread_create_fn g_dump_pthread_create_fn__whitebox
#define g_dump_time_ms_fn g_dump_time_ms_fn__whitebox
#define g_file_size_limit_for_test g_file_size_limit_for_test__whitebox
#define get_timestamp_ms get_timestamp_ms__whitebox
#define get_file_size_limit_bytes get_file_size_limit_bytes__whitebox
#define close_session_file_locked close_session_file_locked__whitebox
#define destroy_session_locked destroy_session_locked__whitebox
#define stop_session_locked stop_session_locked__whitebox
#define check_storage check_storage__whitebox
#define check_enabled check_enabled__whitebox
#define count_active_sessions_locked count_active_sessions_locked__whitebox
#define writer_thread_func writer_thread_func__whitebox
#include "../src/dump_control.c"
#include "../src/dump_writer.c"
#include "../src/dump_pcm_data.c"

int run_whitebox_coverage_shim(void) {
    char buf[DUMP_PCM_KEY_MAX_LEN + 1] = {0};
    char long_path[300] = {0};

    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    dump_key_copy(NULL, "copy-null-dst");
    dump_key_copy(buf, NULL);
    dump_key_escape(NULL, buf, sizeof(buf));
    dump_key_escape(buf, NULL, 0);

    (void)ensure_dir_exists(NULL);
    (void)ensure_dir_exists(long_path);
    close_idle_file_if_needed(NULL);

    atomic_store(&g_init_state, DUMP_INIT_UNINITIALIZED);
    {
        void (*volatile shutdown_fn)(void) = shutdown_module;
        shutdown_fn();
    }

    return 0;
}
