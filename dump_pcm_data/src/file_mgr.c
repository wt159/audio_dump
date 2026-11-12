#include "file_mgr.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "file_mgr_job.h"
#include "key_utils.h"
#include "logger.h"
#include "session.h"

/* Stage-2 lock discipline (docs/stage2-lock-order-spec.md):
 *
 *   L1  F is a leaf lock. Nothing that holds F may take C / S / I, and every
 *       file_mgr_* entry releases F on ALL return paths.
 *   L4  Blocking file I/O never runs while C or F is held.
 *
 * capture_*() takes F, does pure-memory slot work and fills self-contained POD
 * jobs, then releases F. file_mgr_run_job() performs the I/O with no module
 * lock held and may briefly retake F for pure-memory slot accounting. The
 * legacy file_mgr_* entries are a thin compatibility layer for the single-
 * threaded test fixtures; they combine capture + run in that order.
 *
 * Production code must use capture/run directly so the I/O can be placed
 * outside C by the caller (stage-2 PR 2/2). */
static pthread_mutex_t g_file_mgr_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_root_dir[256] = DUMP_ROOT_DIR;
static dump_fwrite_fn_t g_dump_fwrite_fn = fwrite;
static dump_fflush_fn_t g_dump_fflush_fn = fflush;
#if defined(DUMP_PCM_PC) || !defined(ANDROID)
static char g_timestamp_override_for_test[32] = {0};
#endif

typedef struct {
    char name[256];
    time_t mtime;
} file_entry_t;

static uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void get_timestamp_str(char *buf, size_t len) {
#if defined(DUMP_PCM_PC) || !defined(ANDROID)
    if (g_timestamp_override_for_test[0] != '\0') {
        snprintf(buf, len, "%s", g_timestamp_override_for_test);
        return;
    }
#endif

    struct timespec ts;
    struct tm tm_info;
    time_t now = time(NULL);

    localtime_r(&now, &tm_info);
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, len, "%04d%02d%02d_%02d%02d%02d_%03ld",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, ts.tv_nsec / 1000000L);
}

static int ensure_dir_exists(const char *path) {
    char tmp[256];
    size_t len = 0;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(tmp)) {
        return -1;
    }

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    {
        char *scan = tmp + 1;

#if defined(_WIN32)
        /* Skip the "C:" drive prefix: it must never be stat'ed or created. */
        if (len >= 3 &&
            ((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z')) &&
            tmp[1] == ':' && tmp[2] == '/') {
            scan = tmp + 3;
        }
#endif

        for (char *p = scan; *p != '\0'; ++p) {
            struct stat st = {0};
            if (*p != '/') {
                continue;
            }

            *p = '\0';
            if (stat(tmp, &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    LOGE("Path exists but not a directory: %s", tmp);
                    *p = '/';
                    return -1;
                }
            } else if (DUMP_MKDIR(tmp, 0755) != 0 && errno != EEXIST) {
                LOGE("Failed to create directory: %s, errno=%d", tmp, errno);
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }

    {
        struct stat st = {0};
        if (stat(tmp, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                LOGE("Path exists but not a directory: %s", tmp);
                return -1;
            }
            return 0;
        }
    }

    if (DUMP_MKDIR(tmp, 0755) != 0 && errno != EEXIST) {
        LOGE("Failed to create directory: %s, errno=%d", tmp, errno);
        return -1;
    }

    return 0;
}

/* Longest fixed part of a dump file name, excluding the root and the keys:
 * "_<hash8>_<gen>_<split>_<timestamp>.pcm". Ten digits each for generation and
 * split index, nineteen for the timestamp, so 64 leaves room to spare. */
#define DUMP_NAME_TAIL_MAX 64

static int generate_filename(const char *key1, const char *key2, uint32_t generation,
                             uint32_t split_index, char *buf, size_t len) {
    char escaped_key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char escaped_key2[DUMP_PCM_KEY_MAX_LEN + 1];
    char tail[DUMP_NAME_TAIL_MAX];
    char ts[32];
    uint32_t stable_hash = dump_key_hash_pair(key1, key2);
    size_t root_len = 0;
    size_t room = 0;
    size_t key1_len = 0;
    size_t key2_len = 0;
    int tail_len = 0;

    if (buf == NULL || len == 0) {
        return -1;
    }

    dump_key_escape(key1, escaped_key1, sizeof(escaped_key1));
    dump_key_escape(key2, escaped_key2, sizeof(escaped_key2));
    get_timestamp_str(ts, sizeof(ts));

    /* Everything that makes the name unique and makes retention recognise it
     * lives in the tail, so the tail is composed separately and never clipped. */
    tail_len = snprintf(tail, sizeof(tail), "_%08x_%u_%u_%s.pcm", stable_hash, generation,
                        split_index, ts);
    if (tail_len < 0 || (size_t)tail_len >= sizeof(tail)) {
        return -1;
    }

    root_len = strlen(g_root_dir);
    /* NUL, the separator between the keys, and at least one character per key. */
    if (root_len + (size_t)tail_len + 4 > len) {
        LOGE("No room for a dump file name: root=%u tail=%d buffer=%u", (unsigned)root_len,
             tail_len, (unsigned)len);
        return -1;
    }

    room = len - root_len - (size_t)tail_len - 2;
    key1_len = strlen(escaped_key1);
    key2_len = strlen(escaped_key2);
    if (key1_len + key2_len > room) {
        size_t half = room / 2;

        /* Clip the readable part, not the identifying one. A name without the
         * .pcm suffix is invisible to the retention sweep, and without the
         * hash/generation it can collide with a different session and append
         * into the same file. */
        key1_len = (key1_len < half) ? key1_len : half;
        key2_len = (key2_len < room - key1_len) ? key2_len : (room - key1_len);
        LOGW("Dump file name clipped to fit: root=%u keys=%u", (unsigned)root_len,
             (unsigned)(key1_len + key2_len));
    }

    snprintf(buf, len, "%s%.*s_%.*s%s", g_root_dir, (int)key1_len, escaped_key1, (int)key2_len,
             escaped_key2, tail);
    return 0;
}

static file_info_t *find_slot_by_fp(file_manager_t *mgr, FILE *fp) {
    if (mgr == NULL || fp == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            return &mgr->files[i];
        }
    }

    return NULL;
}

/* Pure-memory job builders. These replaced finalize_slot_meta() and
 * close_slot_file(), which used to do fclose + sidecar I/O while holding F. */
static void job_init(fm_job_t *job, fm_job_kind_t kind) {
    memset(job, 0, sizeof(*job));
    job->kind = kind;
    job->rc = -1;
}

static void fill_close_job(const file_info_t *slot, fm_job_t *job) {
    job_init(job, FM_JOB_CLOSE);
    if (slot == NULL || slot->fp == NULL) {
        job->kind = FM_JOB_NONE;
        job->rc = 0;
        return;
    }
    snprintf(job->path, sizeof(job->path), "%s", slot->path);
    job->fp = slot->fp;
}

static void fill_final_job(const file_info_t *slot, fm_job_t *job) {
    job_init(job, FM_JOB_FINAL_META);
    if (slot == NULL || !slot->has_meta) {
        job->has_meta = false;
        return;
    }
    /* Deliberately not gated on the metadata switch: a file that started with
     * a sidecar must finish its lifecycle even if it was disabled meanwhile. */
    snprintf(job->path, sizeof(job->path), "%s", slot->path);
    job->meta = slot->meta;
    job->total_bytes = slot->size;
    job->end_time_unix_ms = sidecar_now_unix_ms();
    job->has_meta = true;
}

/* Called with F held. Emits CLOSE + FINAL jobs for the stalest idle slot and
 * removes the slot from the table; the caller runs the jobs after unlocking.
 * A NULL mgr / NULL out is the whitebox coverage shim's guard. */
static void close_idle_file_if_needed(file_manager_t *mgr,
                                      fm_job_t out[FM_JOB_MAX_PER_STEP],
                                      size_t *out_count) {
    uint64_t now = 0;
    uint64_t stalest_idle_time = 0;
    uint32_t stalest_idx = 0;
    uint32_t i = 0;
    bool found = false;
    file_info_t *slot = NULL;

    if (mgr == NULL || out == NULL || out_count == NULL) {
        return;
    }
    *out_count = 0;
    now = now_mono_ns();

    for (i = 0; i < MAX_OPEN_FILES; ++i) {
        uint64_t idle_time;
        if (!mgr->files[i].in_use) {
            continue;
        }
        idle_time = now - mgr->files[i].last_write_time;
        if (idle_time >= (uint64_t)IDLE_TIMEOUT_SEC * 1000000000ULL && idle_time > stalest_idle_time) {
            stalest_idle_time = idle_time;
            stalest_idx = i;
            found = true;
        }
    }

    if (!found || mgr->count < MAX_OPEN_FILES) {
        return;
    }

    slot = &mgr->files[stalest_idx];
    fill_close_job(slot, &out[(*out_count)++]);
    fill_final_job(slot, &out[(*out_count)++]);
    LOGI("Closed stalest idle file: %s", slot->path);
    slot->fp = NULL;
    slot->in_use = false;
    if (mgr->count > 0) {
        mgr->count--;
    }
}

const char *file_mgr_get_root_dir(void) {
    return g_root_dir;
}

void file_mgr_set_root_dir(const char *dir) {
    size_t len = 0;

    if (dir == NULL || dir[0] == '\0' || strlen(dir) >= sizeof(g_root_dir) - 1) {
        return;
    }

    strncpy(g_root_dir, dir, sizeof(g_root_dir) - 1);
    g_root_dir[sizeof(g_root_dir) - 1] = '\0';
#if defined(_WIN32)
    {
        char *p = g_root_dir;

        for (; *p != '\0'; ++p) {
            if (*p == '\\') {
                *p = '/';
            }
        }
    }
#endif
    len = strlen(g_root_dir);
    if (len > 0 && g_root_dir[len - 1] != '/') {
        g_root_dir[len] = '/';
        g_root_dir[len + 1] = '\0';
    }
}

/* ------------------------------------------------------------------------- *
 * capture: F-held, pure memory, releases F on every path.
 * ------------------------------------------------------------------------- */

void file_mgr_capture_open(file_manager_t *mgr, const session_snapshot_t *snap,
                           fm_job_t out[FM_JOB_MAX_PER_STEP], size_t *out_count) {
    char path[512];
    bool is_new = false;
    size_t n = 0;
    size_t capacity = 0;
    size_t i = 0;

    if (out == NULL || out_count == NULL) {
        return;
    }
    for (i = 0; i < FM_JOB_MAX_PER_STEP; ++i) {
        job_init(&out[i], FM_JOB_NONE);
        out[i].rc = 0;
    }
    *out_count = 0;
    if (mgr == NULL || snap == NULL || out == NULL) {
        return;
    }

    /* Honour the caller's buffer capacity, as the legacy get_handle() did:
     * a tiny buffer must still fail filename generation. */
    capacity = snap->path_capacity;
    if (capacity == 0 || capacity > sizeof(path)) {
        capacity = sizeof(path);
    }

    pthread_mutex_lock(&g_file_mgr_mutex);

    if (snap->file_path[0] == '\0') {
        if (generate_filename(snap->key1, snap->key2, snap->generation,
                              snap->split_index, path, capacity) != 0) {
            goto out;
        }
        is_new = true;
    } else {
        snprintf(path, sizeof(path), "%s", snap->file_path);
    }

    /* An already tracked path needs no fopen: pass its fp through the job. */
    for (i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && strcmp(mgr->files[i].path, path) == 0) {
            fm_job_t *job = &out[n++];
            job_init(job, FM_JOB_OPEN);
            snprintf(job->path, sizeof(job->path), "%s", path);
            job->fp = mgr->files[i].fp;
            job->is_new = false;
            job->rc = 0;
            *out_count = n;
            goto out;
        }
    }

    if (mgr->count >= MAX_OPEN_FILES) {
        fm_job_t evicted[FM_JOB_MAX_PER_STEP];
        size_t evicted_count = 0;
        size_t k = 0;

        close_idle_file_if_needed(mgr, evicted, &evicted_count);
        for (k = 0; k < evicted_count && n < FM_JOB_MAX_PER_STEP; ++k) {
            out[n++] = evicted[k];
        }
        if (mgr->count >= MAX_OPEN_FILES) {
            LOGW("Max open files reached, no idle slot available");
            *out_count = n;
            goto out;
        }
    }

    {
        fm_job_t *job = &out[n++];
        job_init(job, FM_JOB_OPEN);
        snprintf(job->path, sizeof(job->path), "%s", path);
        job->is_new = is_new;
        job->needs_slot_commit = true;
    }
    *out_count = n;

out:
    pthread_mutex_unlock(&g_file_mgr_mutex);
}

void file_mgr_capture_write(file_manager_t *mgr, FILE *fp, const char *data,
                            size_t size, fm_job_t *out) {
    file_info_t *slot = NULL;

    if (out == NULL) {
        return;
    }
    job_init(out, FM_JOB_NONE);
    if (mgr == NULL || fp == NULL || data == NULL || size == 0) {
        out->rc = 0;
        return;
    }

    job_init(out, FM_JOB_WRITE);
    out->fp = fp;
    out->payload = data;
    out->payload_size = size;

    /* Pure-memory slot lookup under F: decide whether run must commit the
     * written size. A foreign fp still gets its fwrite (legacy behaviour), it
     * simply has no slot to account for. */
    pthread_mutex_lock(&g_file_mgr_mutex);
    slot = find_slot_by_fp(mgr, fp);
    out->needs_slot_commit = (slot != NULL);
    pthread_mutex_unlock(&g_file_mgr_mutex);
}

size_t file_mgr_capture_close(file_manager_t *mgr, FILE *fp,
                              fm_job_t out[FM_JOB_MAX_PER_STEP]) {
    size_t n = 0;
    size_t i = 0;

    if (out == NULL) {
        return 0;
    }
    for (i = 0; i < FM_JOB_MAX_PER_STEP; ++i) {
        job_init(&out[i], FM_JOB_NONE);
        out[i].rc = 0;
    }
    if (mgr == NULL || fp == NULL) {
        return 0;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    for (i = 0; i < MAX_OPEN_FILES; ++i) {
        file_info_t *slot = &mgr->files[i];

        if (slot->in_use && slot->fp == fp) {
            LOGI("File closed: %s", slot->path);
            fill_close_job(slot, &out[n++]);
            fill_final_job(slot, &out[n++]);
            slot->fp = NULL;
            slot->in_use = false;
            if (mgr->count > 0) {
                mgr->count--;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_file_mgr_mutex);
    return n;
}

void file_mgr_capture_initial_meta(file_manager_t *mgr, FILE *fp,
                                   const dump_pcm_file_meta_t *meta,
                                   bool write_initial, fm_job_t *out) {
    file_info_t *slot = NULL;

    if (out == NULL) {
        return;
    }
    job_init(out, FM_JOB_NONE);
    out->rc = 0;
    if (mgr == NULL || fp == NULL) {
        return;
    }

    /* Single F-held region with one exit, so no early-return branch can skip
     * the unlock (the historical failure mode in get_handle/attach_meta). */
    pthread_mutex_lock(&g_file_mgr_mutex);
    slot = find_slot_by_fp(mgr, fp);
    if (slot != NULL && meta != NULL && sidecar_is_enabled()) {
        if (!slot->has_meta) {
            slot->meta = *meta;
            slot->has_meta = true;
        }
        if (write_initial && session_format_is_valid(&slot->meta.format)) {
            job_init(out, FM_JOB_INITIAL_META);
            snprintf(out->path, sizeof(out->path), "%s", slot->path);
            out->meta = slot->meta;
            out->has_meta = true;
        }
    }
    pthread_mutex_unlock(&g_file_mgr_mutex);
}

void file_mgr_capture_cleanup(file_manager_t *mgr, uint32_t max_files,
                              uint32_t max_days, fm_job_t *out) {
    (void)mgr; /* cleanup only needs the process-wide root, not the slot table */

    if (out == NULL) {
        return;
    }
    job_init(out, FM_JOB_CLEANUP);

    pthread_mutex_lock(&g_file_mgr_mutex);
    snprintf(out->path, sizeof(out->path), "%s", g_root_dir);
    pthread_mutex_unlock(&g_file_mgr_mutex);

    out->max_files = max_files;
    out->max_days = max_days;
}

void file_mgr_capture_ensure_dir(fm_job_t *out) {
    if (out == NULL) {
        return;
    }
    job_init(out, FM_JOB_ENSURE_DIR);

    pthread_mutex_lock(&g_file_mgr_mutex);
    snprintf(out->path, sizeof(out->path), "%s", g_root_dir);
    pthread_mutex_unlock(&g_file_mgr_mutex);
}

bool file_mgr_slot_tracked(file_manager_t *mgr, FILE *fp) {
    bool tracked = false;
    uint32_t i = 0;

    if (mgr == NULL || fp == NULL) {
        return false;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    for (i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            tracked = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_file_mgr_mutex);
    return tracked;
}

/* ------------------------------------------------------------------------- *
 * run: no module lock held on entry. Blocking I/O lives exclusively here.
 * ------------------------------------------------------------------------- */

/* Only our own artifacts take part in the retention policy. A substring match
 * would also catch host files such as "mix.pcm.bak" once the root directory is
 * configurable. */
static bool has_pcm_suffix(const char *name) {
    size_t len = 0;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".pcm") == 0;
}

static int run_cleanup(const char *root, uint32_t max_files, uint32_t max_days) {
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    file_entry_t *files = NULL;
    uint32_t file_count = 0;
    uint32_t capacity = 0;
    time_t now = 0;
    time_t cutoff = 0;
    uint32_t keep = 0;

    dir = opendir(root);
    if (dir == NULL) {
        LOGE("Failed to open directory: %s", root);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char filepath[512];
        struct stat st = {0};

        if (!has_pcm_suffix(entry->d_name)) {
            continue;
        }

        if (file_count >= capacity) {
            file_entry_t *grown = NULL;
            capacity = (capacity == 0) ? 32 : capacity * 2;
            grown = realloc(files, capacity * sizeof(*files));
            if (grown == NULL) {
                free(files);
                closedir(dir);
                return -1;
            }
            files = grown;
        }

        snprintf(filepath, sizeof(filepath), "%s%s", root, entry->d_name);
        if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        strncpy(files[file_count].name, entry->d_name, sizeof(files[file_count].name) - 1);
        files[file_count].name[sizeof(files[file_count].name) - 1] = '\0';
        files[file_count].mtime = st.st_mtime;
        file_count++;
    }

    closedir(dir);
    if (file_count <= max_files) {
        free(files);
        return 0;
    }

    for (uint32_t i = 0; i < file_count; ++i) {
        for (uint32_t j = i + 1; j < file_count; ++j) {
            if (files[i].mtime < files[j].mtime) {
                file_entry_t tmp = files[i];
                files[i] = files[j];
                files[j] = tmp;
            }
        }
    }

    now = time(NULL);
    cutoff = now - (time_t)max_days * 24 * 3600;
    keep = (max_files < file_count) ? max_files : file_count;
    for (uint32_t i = keep; i < file_count; ++i) {
        char filepath[512];
        if (files[i].mtime >= cutoff) {
            continue;
        }
        snprintf(filepath, sizeof(filepath), "%s%s", root, files[i].name);
        if (remove(filepath) == 0) {
            LOGI("Removed old file: %s", filepath);
        }
    }

    free(files);
    return 0;
}

int file_mgr_run_job(file_manager_t *mgr, fm_job_t *job) {
    if (job == NULL) {
        return -1;
    }

    switch (job->kind) {
    case FM_JOB_NONE:
        job->rc = 0;
        return 0;

    case FM_JOB_OPEN: {
        FILE *fp = NULL;
        struct stat st = {0};
        uint64_t size = 0;
        uint32_t i = 0;

        /* A reused tracked slot needs no fopen. */
        if (job->fp != NULL) {
            job->rc = 0;
            return 0;
        }

        fp = fopen(job->path, "ab");
        if (fp == NULL) {
            LOGE("Failed to open file: %s, errno=%d", job->path, errno);
            job->rc = -1;
            return -1;
        }
        if (stat(job->path, &st) == 0) {
            size = (uint64_t)st.st_size;
        }

        if (mgr == NULL) {
            fclose(fp);
            job->rc = -1;
            return -1;
        }

        /* Short F-held, pure-memory slot registration. No I/O in here. */
        pthread_mutex_lock(&g_file_mgr_mutex);
        for (i = 0; i < MAX_OPEN_FILES; ++i) {
            file_info_t *slot = &mgr->files[i];
            if (slot->in_use) {
                continue;
            }
            snprintf(slot->path, sizeof(slot->path), "%s", job->path);
            slot->fp = fp;
            slot->size = size;
            slot->last_write_time = now_mono_ns();
            slot->in_use = true;
            slot->has_meta = false;
            memset(&slot->meta, 0, sizeof(slot->meta));
            mgr->count++;
            break;
        }
        pthread_mutex_unlock(&g_file_mgr_mutex);

        if (i == MAX_OPEN_FILES) {
            fclose(fp);
            job->fp = NULL;
            job->rc = -1;
            return -1;
        }

        job->fp = fp;
        job->rc = 0;
        LOGI("File opened: %s", job->path);
        return 0;
    }

    case FM_JOB_WRITE: {
        if (g_dump_fwrite_fn(job->payload, 1, job->payload_size, job->fp) != job->payload_size) {
            LOGE("Failed to write to file: expected=%zu", job->payload_size);
            job->rc = -1;
            return -1;
        }
        if (g_dump_fflush_fn(job->fp) != 0) {
            LOGE("Failed to flush file after write");
            job->rc = -1;
            return -1;
        }

        if (job->needs_slot_commit && mgr != NULL) {
            pthread_mutex_lock(&g_file_mgr_mutex);
            for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
                file_info_t *slot = &mgr->files[i];
                if (slot->in_use && slot->fp == job->fp) {
                    slot->size += job->payload_size;
                    slot->last_write_time = now_mono_ns();
                    break;
                }
            }
            pthread_mutex_unlock(&g_file_mgr_mutex);
        }
        job->rc = 0;
        return 0;
    }

    case FM_JOB_CLOSE:
        if (job->fp == NULL) {
            job->rc = 0;
            return 0;
        }
        if (fclose(job->fp) != 0) {
            LOGW("Failed to close file: %s", job->path);
            job->fp = NULL;
            job->rc = -1;
            return -1;
        }
        job->fp = NULL;
        job->rc = 0;
        return 0;

    case FM_JOB_INITIAL_META:
    case FM_JOB_FINAL_META: {
        bool complete = (job->kind == FM_JOB_FINAL_META);
        if (job->has_meta &&
            sidecar_write_meta(job->path, &job->meta, complete,
                               job->total_bytes, job->end_time_unix_ms) != 0) {
            LOGW("%s sidecar: %s", complete ? "Failed to finalize" : "Failed to write initial",
                 job->path);
        }
        /* Sidecar failure never changes the job's success semantics. */
        job->rc = 0;
        return 0;
    }

    case FM_JOB_CLEANUP:
        job->rc = run_cleanup(job->path, job->max_files, job->max_days);
        return job->rc;

    case FM_JOB_ENSURE_DIR:
        job->rc = ensure_dir_exists(job->path);
        return job->rc;

    default:
        job->rc = -1;
        return -1;
    }
}

/* A CLOSE job's rc gates its paired FINAL_META job: a failed fclose means the
 * final size is unknown, so the sidecar must not be marked complete. */
static void run_job_sequence(file_manager_t *mgr, fm_job_t *jobs, size_t count) {
    size_t i = 0;

    for (i = 0; i < count; ++i) {
        if (jobs[i].kind == FM_JOB_FINAL_META && i > 0 &&
            jobs[i - 1].kind == FM_JOB_CLOSE && jobs[i - 1].rc != 0) {
            continue;
        }
        file_mgr_run_job(mgr, &jobs[i]);
    }
}

/* ------------------------------------------------------------------------- *
 * Compatibility layer: legacy signatures, test fixtures only.
 * Each entry is capture (F released before return) followed by run (no lock).
 * ------------------------------------------------------------------------- */

int file_mgr_init(file_manager_t *mgr) {
    fm_job_t job;

    if (mgr == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    memset(mgr, 0, sizeof(*mgr));
    pthread_mutex_unlock(&g_file_mgr_mutex);

    file_mgr_capture_ensure_dir(&job);
    return file_mgr_run_job(mgr, &job);
}

void file_mgr_destroy(file_manager_t *mgr) {
    fm_job_t jobs[MAX_OPEN_FILES * 2];
    size_t count = 0;
    uint32_t s = 0;

    if (mgr == NULL) {
        return;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    for (s = 0; s < MAX_OPEN_FILES; ++s) {
        file_info_t *slot = &mgr->files[s];
        if (!slot->in_use || slot->fp == NULL) {
            continue;
        }
        fill_close_job(slot, &jobs[count++]);
        fill_final_job(slot, &jobs[count++]);
        slot->fp = NULL;
        slot->in_use = false;
        if (mgr->count > 0) {
            mgr->count--;
        }
    }
    memset(mgr, 0, sizeof(*mgr));
    pthread_mutex_unlock(&g_file_mgr_mutex);

    run_job_sequence(mgr, jobs, count);
}

FILE *file_mgr_get_handle(file_manager_t *mgr, const char *key1, const char *key2,
                          uint32_t generation, uint32_t split_index,
                          char *path_buf, size_t path_buf_size, bool *is_new) {
    session_snapshot_t snap;
    fm_job_t jobs[FM_JOB_MAX_PER_STEP];
    size_t count = 0;
    size_t i = 0;
    FILE *result = NULL;
    bool new_file = false;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2) ||
        path_buf == NULL || path_buf_size == 0 || is_new == NULL) {
        return NULL;
    }

    memset(&snap, 0, sizeof(snap));
    dump_key_copy(snap.key1, key1);
    dump_key_copy(snap.key2, key2);
    snap.generation = generation;
    snap.split_index = split_index;
    snap.path_capacity = path_buf_size;
    snprintf(snap.file_path, sizeof(snap.file_path), "%s", path_buf);

    file_mgr_capture_open(mgr, &snap, jobs, &count);
    run_job_sequence(mgr, jobs, count);

    for (i = 0; i < count; ++i) {
        if (jobs[i].kind == FM_JOB_OPEN) {
            result = jobs[i].fp;
            new_file = jobs[i].is_new;
            snprintf(path_buf, path_buf_size, "%s", jobs[i].path);
        }
    }

    *is_new = new_file;
    return result;
}

int file_mgr_write(file_manager_t *mgr, FILE *fp, const char *data, size_t size) {
    fm_job_t job;

    if (mgr == NULL || fp == NULL || data == NULL || size == 0) {
        return -1;
    }

    file_mgr_capture_write(mgr, fp, data, size, &job);
    if (job.kind == FM_JOB_NONE) {
        return -1;
    }
    return file_mgr_run_job(mgr, &job);
}

int file_mgr_close(file_manager_t *mgr, FILE *fp) {
    fm_job_t jobs[FM_JOB_MAX_PER_STEP];
    size_t count = 0;

    if (mgr == NULL || fp == NULL) {
        return -1;
    }

    count = file_mgr_capture_close(mgr, fp, jobs);
    if (count == 0) {
        return -1;
    }
    run_job_sequence(mgr, jobs, count);
    return 0;
}

void file_mgr_attach_meta(file_manager_t *mgr, FILE *fp, const dump_pcm_file_meta_t *meta,
                          bool write_initial) {
    fm_job_t job;

    if (mgr == NULL || fp == NULL) {
        return;
    }

    file_mgr_capture_initial_meta(mgr, fp, meta, write_initial, &job);
    file_mgr_run_job(mgr, &job);
}

bool file_mgr_is_tracking(file_manager_t *mgr, FILE *fp) {
    return file_mgr_slot_tracked(mgr, fp);
}

int file_mgr_cleanup(uint32_t max_files, uint32_t max_days) {
    fm_job_t job;

    file_mgr_capture_cleanup(NULL, max_files, max_days, &job);
    return file_mgr_run_job(NULL, &job);
}

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
void file_mgr_set_fwrite_fn_for_test(dump_fwrite_fn_t fn) {
    g_dump_fwrite_fn = (fn != NULL) ? fn : fwrite;
}

void file_mgr_set_fflush_fn_for_test(dump_fflush_fn_t fn) {
    g_dump_fflush_fn = (fn != NULL) ? fn : fflush;
}

void file_mgr_set_timestamp_override_for_test(const char *timestamp) {
    if (timestamp == NULL) {
        g_timestamp_override_for_test[0] = '\0';
        return;
    }

    strncpy(g_timestamp_override_for_test, timestamp, sizeof(g_timestamp_override_for_test) - 1);
    g_timestamp_override_for_test[sizeof(g_timestamp_override_for_test) - 1] = '\0';
}
#endif
