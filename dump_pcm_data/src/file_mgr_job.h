#ifndef DUMP_PCM_FILE_MGR_JOB_H
#define DUMP_PCM_FILE_MGR_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "file_mgr.h"
#include "sidecar.h"

/* Stage-2 job model (docs/stage2-lock-order-spec.md, invariants L1/L2/L4).
 *
 * capture_*() takes F, performs pure-memory slot work, fills POD jobs and
 * releases F on EVERY return path. file_mgr_run_job() performs the blocking
 * I/O without holding C / F / S / I, and may briefly retake F for pure-memory
 * slot accounting. A job never dereferences a session and never reads the
 * slot table during run, so a concurrent close cannot invalidate it. */

typedef enum {
    FM_JOB_NONE = 0,
    FM_JOB_OPEN,           /* fopen(path,"ab") + register the slot (short F) */
    FM_JOB_WRITE,          /* fwrite + fflush + slot accounting (short F) */
    FM_JOB_CLOSE,          /* fclose */
    FM_JOB_INITIAL_META,   /* incomplete sidecar */
    FM_JOB_FINAL_META,     /* complete sidecar */
    FM_JOB_CLEANUP,        /* opendir/stat/remove retention sweep */
    FM_JOB_ENSURE_DIR      /* mkdir for the dump root */
} fm_job_kind_t;

/* Evict CLOSE + evict FINAL + OPEN. */
#define FM_JOB_MAX_PER_STEP 3

typedef struct {
    fm_job_kind_t kind;
    /* 512 rather than the spec's 256: the legacy get_handle() contract honours
     * the caller's path buffer capacity, so capture must be able to carry a
     * name generated for a 256/300-byte caller buffer without truncating it. */
    char path[512];
    FILE *fp;
    const char *payload;      /* borrowed; valid until run returns */
    size_t payload_size;
    dump_pcm_file_meta_t meta;
    uint64_t total_bytes;
    uint64_t end_time_unix_ms;
    uint32_t max_files;
    uint32_t max_days;
    bool is_new;
    bool needs_slot_commit;
    bool has_meta;
    int rc;                   /* written by run */
} fm_job_t;

/* Copied under C in the writer's decision phase. fp_tag is an IDENTITY LABEL
 * only: it is compared on publish, never used as a write target. */
typedef struct {
    char key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char key2[DUMP_PCM_KEY_MAX_LEN + 1];
    uint32_t generation;
    uint32_t split_index;
    char file_path[256];
    uint64_t file_size;
    uint64_t file_start_unix_ms;
    bool has_format;
    dump_pcm_format_t format;
    FILE *fp_tag;
    uint32_t pending_blocks;
    int state;
    /* Capacity of file_path as seen by the caller. generate_filename() uses it
     * to clip long names, so the compatibility layer must carry it through. */
    size_t path_capacity;
} session_snapshot_t;

/* All capture_* take F internally and release it before returning, including
 * early-return paths. out[] entries with kind == FM_JOB_NONE are no-ops. */
void file_mgr_capture_open(file_manager_t *mgr, const session_snapshot_t *snap,
                           fm_job_t out[FM_JOB_MAX_PER_STEP], size_t *out_count);
void file_mgr_capture_write(file_manager_t *mgr, FILE *fp, const char *data,
                            size_t size, fm_job_t *out);
size_t file_mgr_capture_close(file_manager_t *mgr, FILE *fp,
                              fm_job_t out[FM_JOB_MAX_PER_STEP]);
/* write_initial preserves the legacy attach_meta(..., bool) policy: a reopened
 * file must not downgrade a completed sidecar. */
void file_mgr_capture_initial_meta(file_manager_t *mgr, FILE *fp,
                                   const dump_pcm_file_meta_t *meta,
                                   bool write_initial, fm_job_t *out);
void file_mgr_capture_cleanup(file_manager_t *mgr, uint32_t max_files,
                              uint32_t max_days, fm_job_t *out);
void file_mgr_capture_ensure_dir(fm_job_t *out);
bool file_mgr_slot_tracked(file_manager_t *mgr, FILE *fp);

/* Caller must hold NO module lock. */
int file_mgr_run_job(file_manager_t *mgr, fm_job_t *job);

#endif /* DUMP_PCM_FILE_MGR_JOB_H */
