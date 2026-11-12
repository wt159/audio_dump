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

#include "key_utils.h"
#include "logger.h"
#include "session.h"

/* File manager state is touched from two threads: the writer (open, write,
 * close, sweep) and the caller (closing a session's file on stop). Until now it
 * was protected by whatever lock the caller happened to hold, which only worked
 * because both threads happened to use the same one. The manager owns its own
 * lock instead, so file I/O can move out from under the module mutex in a later
 * change without re-deriving which call sites need it.
 *
 * Never held while acquiring another lock, and never acquired while holding one:
 * every other lock in the module is taken before entering this component. */
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

/* Called just before a slot is released, whatever the reason: explicit close,
 * rollover, size split, LRU eviction or module teardown. The slot still holds
 * its path, size and metadata copy, so the final version is self-contained. */
static void finalize_slot_meta(file_info_t *slot) {
    if (slot == NULL || !slot->has_meta) {
        return;
    }

    /* Deliberately not gated on the switch: a file that started with a sidecar
     * must finish its lifecycle even if metadata was disabled meanwhile. */
    if (sidecar_write_meta(slot->path, &slot->meta, true, slot->size, sidecar_now_unix_ms()) != 0) {
        LOGW("Failed to finalize sidecar: %s", slot->path);
    }
}

/* Closes a slot's stream and reports whether the data is safely on disk. A
 * failed fclose means the final size is unknown, so the caller must not mark
 * the sidecar as complete. */
static bool close_slot_file(file_info_t *slot) {
    if (slot == NULL || slot->fp == NULL) {
        return false;
    }

    if (fclose(slot->fp) != 0) {
        LOGW("Failed to close file: %s", slot->path);
        slot->fp = NULL;
        return false;
    }

    slot->fp = NULL;
    return true;
}

static void close_idle_file_if_needed(file_manager_t *mgr) {
    uint64_t now = now_mono_ns();
    uint64_t stalest_idle_time = 0;
    uint32_t stalest_idx = 0;
    bool found = false;

    if (mgr == NULL) {
        return;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!mgr->files[i].in_use) {
            continue;
        }

        uint64_t idle_time = now - mgr->files[i].last_write_time;
        if (idle_time >= (uint64_t)IDLE_TIMEOUT_SEC * 1000000000ULL && idle_time > stalest_idle_time) {
            stalest_idle_time = idle_time;
            stalest_idx = i;
            found = true;
        }
    }

    if (found && mgr->count >= MAX_OPEN_FILES) {
        if (close_slot_file(&mgr->files[stalest_idx])) {
            finalize_slot_meta(&mgr->files[stalest_idx]);
        }
        mgr->files[stalest_idx].in_use = false;
        mgr->count--;
        LOGI("Closed stalest idle file: %s", mgr->files[stalest_idx].path);
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

int file_mgr_init(file_manager_t *mgr) {
    int rc = -1;

    if (mgr == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    memset(mgr, 0, sizeof(*mgr));
    rc = ensure_dir_exists(g_root_dir);
    pthread_mutex_unlock(&g_file_mgr_mutex);
    return rc;
}

void file_mgr_destroy(file_manager_t *mgr) {
    if (mgr == NULL) {
        return;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp != NULL) {
            if (close_slot_file(&mgr->files[i])) {
                finalize_slot_meta(&mgr->files[i]);
            }
        }
    }

    memset(mgr, 0, sizeof(*mgr));
    pthread_mutex_unlock(&g_file_mgr_mutex);
}

FILE *file_mgr_get_handle(file_manager_t *mgr, const char *key1, const char *key2,
                          uint32_t generation, uint32_t split_index,
                          char *path_buf, size_t path_buf_size, bool *is_new) {
    FILE *fp = NULL;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2) ||
        path_buf == NULL || path_buf_size == 0 || is_new == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);

    *is_new = false;
    if (path_buf[0] == '\0') {
        if (generate_filename(key1, key2, generation, split_index, path_buf, path_buf_size) != 0) {
            pthread_mutex_unlock(&g_file_mgr_mutex);
            return NULL;
        }
        *is_new = true;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && strcmp(mgr->files[i].path, path_buf) == 0) {
            *is_new = false;
            pthread_mutex_unlock(&g_file_mgr_mutex);
            return mgr->files[i].fp;
        }
    }

    if (mgr->count >= MAX_OPEN_FILES) {
        close_idle_file_if_needed(mgr);
        if (mgr->count >= MAX_OPEN_FILES) {
            LOGW("Max open files reached, no idle slot available");
            pthread_mutex_unlock(&g_file_mgr_mutex);
            return NULL;
        }
    }

    fp = fopen(path_buf, "ab");
    if (fp == NULL) {
        LOGE("Failed to open file: %s, errno=%d", path_buf, errno);
        pthread_mutex_unlock(&g_file_mgr_mutex);
        return NULL;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!mgr->files[i].in_use) {
            struct stat st = {0};
            strncpy(mgr->files[i].path, path_buf, sizeof(mgr->files[i].path) - 1);
            mgr->files[i].path[sizeof(mgr->files[i].path) - 1] = '\0';
            mgr->files[i].fp = fp;
            mgr->files[i].last_write_time = now_mono_ns();
            mgr->files[i].in_use = true;
            /* A recycled slot must not inherit the previous file's sidecar. */
            mgr->files[i].has_meta = false;
            memset(&mgr->files[i].meta, 0, sizeof(mgr->files[i].meta));
            if (stat(path_buf, &st) == 0) {
                mgr->files[i].size = (uint64_t)st.st_size;
            }
            mgr->count++;
            LOGI("File opened: %s", path_buf);
            pthread_mutex_unlock(&g_file_mgr_mutex);
            return fp;
        }
    }

    fclose(fp);
    pthread_mutex_unlock(&g_file_mgr_mutex);
    return NULL;
}

int file_mgr_write(file_manager_t *mgr, FILE *fp, const char *data, size_t size) {
    int rc = -1;

    if (mgr == NULL || fp == NULL || data == NULL || size == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    if (g_dump_fwrite_fn(data, 1, size, fp) != size) {
        LOGE("Failed to write to file: expected=%zu", size);
        goto out;
    }
    if (g_dump_fflush_fn(fp) != 0) {
        LOGE("Failed to flush file after write");
        goto out;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            mgr->files[i].size += size;
            mgr->files[i].last_write_time = now_mono_ns();
            break;
        }
    }

    rc = 0;

out:
    pthread_mutex_unlock(&g_file_mgr_mutex);
    return rc;
}

int file_mgr_close(file_manager_t *mgr, FILE *fp) {
    int rc = -1;

    if (mgr == NULL || fp == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            if (close_slot_file(&mgr->files[i])) {
                finalize_slot_meta(&mgr->files[i]);
            }
            mgr->files[i].in_use = false;
            mgr->count--;
            LOGI("File closed: %s", mgr->files[i].path);
            rc = 0;
            break;
        }
    }

    pthread_mutex_unlock(&g_file_mgr_mutex);
    return rc;
}

void file_mgr_attach_meta(file_manager_t *mgr, FILE *fp, const dump_pcm_file_meta_t *meta,
                          bool write_initial) {
    file_info_t *slot = NULL;

    if (mgr == NULL || fp == NULL) {
        return;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    slot = find_slot_by_fp(mgr, fp);

    if (slot == NULL || meta == NULL || !sidecar_is_enabled()) {
        pthread_mutex_unlock(&g_file_mgr_mutex);
        return;
    }

    if (!slot->has_meta) {
        slot->meta = *meta;
        slot->has_meta = true;
    }
    if (!write_initial || !session_format_is_valid(&slot->meta.format)) {
        pthread_mutex_unlock(&g_file_mgr_mutex);
        return;
    }

    if (sidecar_write_meta(slot->path, &slot->meta, false, 0, 0) != 0) {
        LOGW("Failed to write initial sidecar: %s", slot->path);
    }
    pthread_mutex_unlock(&g_file_mgr_mutex);
}

bool file_mgr_is_tracking(file_manager_t *mgr, FILE *fp) {
    bool tracked = false;

    if (mgr == NULL || fp == NULL) {
        return false;
    }

    pthread_mutex_lock(&g_file_mgr_mutex);
    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            tracked = true;
            break;
        }
    }

    pthread_mutex_unlock(&g_file_mgr_mutex);
    return tracked;
}

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

int file_mgr_cleanup(uint32_t max_files, uint32_t max_days) {
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    file_entry_t *files = NULL;
    uint32_t file_count = 0;
    uint32_t capacity = 0;
    time_t now = 0;
    time_t cutoff = 0;
    uint32_t keep = 0;

    pthread_mutex_lock(&g_file_mgr_mutex);
    dir = opendir(g_root_dir);
    if (dir == NULL) {
        LOGE("Failed to open directory: %s", g_root_dir);
        pthread_mutex_unlock(&g_file_mgr_mutex);
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
                pthread_mutex_unlock(&g_file_mgr_mutex);
                return -1;
            }
            files = grown;
        }

        snprintf(filepath, sizeof(filepath), "%s%s", g_root_dir, entry->d_name);
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
        pthread_mutex_unlock(&g_file_mgr_mutex);
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
        snprintf(filepath, sizeof(filepath), "%s%s", g_root_dir, files[i].name);
        if (remove(filepath) == 0) {
            LOGI("Removed old file: %s", filepath);
        }
    }

    free(files);
    pthread_mutex_unlock(&g_file_mgr_mutex);
    return 0;
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
