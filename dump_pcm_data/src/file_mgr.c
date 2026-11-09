#include "file_mgr.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "key_utils.h"
#include "logger.h"

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

    for (char *p = tmp + 1; *p != '\0'; ++p) {
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

static void generate_filename(const char *key1, const char *key2, uint32_t generation,
                              uint32_t split_index, char *buf, size_t len) {
    char escaped_key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char escaped_key2[DUMP_PCM_KEY_MAX_LEN + 1];
    char ts[32];
    uint32_t stable_hash = dump_key_hash_pair(key1, key2);

    dump_key_escape(key1, escaped_key1, sizeof(escaped_key1));
    dump_key_escape(key2, escaped_key2, sizeof(escaped_key2));
    get_timestamp_str(ts, sizeof(ts));
    snprintf(buf, len, "%s%s_%s_%08x_%u_%u_%s.pcm", g_root_dir, escaped_key1, escaped_key2,
             stable_hash, generation, split_index, ts);
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
        fclose(mgr->files[stalest_idx].fp);
        mgr->files[stalest_idx].fp = NULL;
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
    len = strlen(g_root_dir);
    if (len > 0 && g_root_dir[len - 1] != '/') {
        g_root_dir[len] = '/';
        g_root_dir[len + 1] = '\0';
    }
}

int file_mgr_init(file_manager_t *mgr) {
    if (mgr == NULL) {
        return -1;
    }

    memset(mgr, 0, sizeof(*mgr));
    return ensure_dir_exists(g_root_dir);
}

void file_mgr_destroy(file_manager_t *mgr) {
    if (mgr == NULL) {
        return;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp != NULL) {
            fclose(mgr->files[i].fp);
        }
    }

    memset(mgr, 0, sizeof(*mgr));
}

FILE *file_mgr_get_handle(file_manager_t *mgr, const char *key1, const char *key2,
                          uint32_t generation, uint32_t split_index,
                          char *path_buf, size_t path_buf_size, bool *is_new) {
    FILE *fp = NULL;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2) ||
        path_buf == NULL || path_buf_size == 0 || is_new == NULL) {
        return NULL;
    }

    *is_new = false;
    if (path_buf[0] == '\0') {
        generate_filename(key1, key2, generation, split_index, path_buf, path_buf_size);
        *is_new = true;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && strcmp(mgr->files[i].path, path_buf) == 0) {
            *is_new = false;
            return mgr->files[i].fp;
        }
    }

    if (mgr->count >= MAX_OPEN_FILES) {
        close_idle_file_if_needed(mgr);
        if (mgr->count >= MAX_OPEN_FILES) {
            LOGW("Max open files reached, no idle slot available");
            return NULL;
        }
    }

    fp = fopen(path_buf, "ab");
    if (fp == NULL) {
        LOGE("Failed to open file: %s, errno=%d", path_buf, errno);
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
            if (stat(path_buf, &st) == 0) {
                mgr->files[i].size = (uint64_t)st.st_size;
            }
            mgr->count++;
            LOGI("File opened: %s", path_buf);
            return fp;
        }
    }

    fclose(fp);
    return NULL;
}

int file_mgr_write(file_manager_t *mgr, FILE *fp, const char *data, size_t size) {
    if (mgr == NULL || fp == NULL || data == NULL || size == 0) {
        return -1;
    }

    if (g_dump_fwrite_fn(data, 1, size, fp) != size) {
        LOGE("Failed to write to file: expected=%zu", size);
        return -1;
    }
    if (g_dump_fflush_fn(fp) != 0) {
        LOGE("Failed to flush file after write");
        return -1;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            mgr->files[i].size += size;
            mgr->files[i].last_write_time = now_mono_ns();
            break;
        }
    }

    return 0;
}

int file_mgr_close(file_manager_t *mgr, FILE *fp) {
    if (mgr == NULL || fp == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            fclose(fp);
            mgr->files[i].fp = NULL;
            mgr->files[i].in_use = false;
            mgr->count--;
            LOGI("File closed: %s", mgr->files[i].path);
            return 0;
        }
    }

    return -1;
}

bool file_mgr_is_tracking(file_manager_t *mgr, FILE *fp) {
    if (mgr == NULL || fp == NULL) {
        return false;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && mgr->files[i].fp == fp) {
            return true;
        }
    }

    return false;
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

    dir = opendir(g_root_dir);
    if (dir == NULL) {
        LOGE("Failed to open directory: %s", g_root_dir);
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
