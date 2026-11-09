#ifndef DUMP_PCM_FILE_MGR_H
#define DUMP_PCM_FILE_MGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "platform_config.h"
#include "sidecar.h"

typedef struct {
    char path[256];
    FILE *fp;
    uint64_t size;
    uint64_t last_write_time;
    bool in_use;
    /* Self-contained copy so the closing hook never dereferences a session
     * that may already be gone (LRU eviction is invisible to the writer). */
    dump_pcm_file_meta_t meta;
    bool has_meta;
} file_info_t;

typedef struct {
    file_info_t files[MAX_OPEN_FILES];
    uint32_t count;
} file_manager_t;

int file_mgr_init(file_manager_t *mgr);
void file_mgr_destroy(file_manager_t *mgr);
FILE *file_mgr_get_handle(file_manager_t *mgr, const char *key1, const char *key2,
                          uint32_t generation, uint32_t split_index,
                          char *path_buf, size_t path_buf_size, bool *is_new);
int file_mgr_write(file_manager_t *mgr, FILE *fp, const char *data, size_t size);
int file_mgr_close(file_manager_t *mgr, FILE *fp);

/* Copies meta onto the slot that owns fp. write_initial is set for a freshly
 * created file: it writes the first (incomplete) version immediately. A reopen
 * passes false so the completed version is not downgraded; closing the file
 * finalises it in both cases. */
void file_mgr_attach_meta(file_manager_t *mgr, FILE *fp, const dump_pcm_file_meta_t *meta,
                          bool write_initial);
bool file_mgr_is_tracking(file_manager_t *mgr, FILE *fp);
int file_mgr_cleanup(uint32_t max_files, uint32_t max_days);
const char *file_mgr_get_root_dir(void);
void file_mgr_set_root_dir(const char *dir);

typedef size_t (*dump_fwrite_fn_t)(const void *ptr, size_t size, size_t nmemb, FILE *stream);
typedef int (*dump_fflush_fn_t)(FILE *stream);

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
void file_mgr_set_fwrite_fn_for_test(dump_fwrite_fn_t fn);
void file_mgr_set_fflush_fn_for_test(dump_fflush_fn_t fn);
void file_mgr_set_timestamp_override_for_test(const char *timestamp);
#endif

#endif
