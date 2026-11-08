#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define DUMP_PCM_INTERNAL_DEBUG_API 1

#include "../include/dump_pcm_data.h"
#include "../src/file_mgr.h"
#include "../src/key_utils.h"
#include "../src/logger.h"
#include "../src/queue.h"
#include "../src/session.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
void dump_pcm_set_file_size_limit_for_test(uint64_t bytes);
#endif

int run_whitebox_coverage_shim(void);

static int tests_passed = 0;
static int tests_failed = 0;
static char g_test_dir_base[256] = {0};
static int g_test_dir_seq = 0;

static void pc_log_fn(log_level_t level, const char *tag, const char *fmt, ...) {
    const char *level_str[] = {"ERROR", "WARN", "INFO"};
    va_list ap;

    printf("[%s][%s] ", level_str[level], tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void pass(const char *name) {
    printf("PASS: %s\n", name);
    tests_passed++;
}

static void fail(const char *name, const char *why) {
    printf("FAIL: %s, %s\n", name, why);
    tests_failed++;
}

static int fake_statvfs_fail(const char *path, struct statvfs *buf) {
    (void)path;
    (void)buf;
    errno = EIO;
    return -1;
}

static int fake_statvfs_low(const char *path, struct statvfs *buf) {
    (void)path;
    memset(buf, 0, sizeof(*buf));
    buf->f_bavail = 1;
    buf->f_frsize = 1024 * 1024;
    return 0;
}

static int fake_statvfs_large(const char *path, struct statvfs *buf) {
    (void)path;
    memset(buf, 0, sizeof(*buf));
    buf->f_bavail = 33792;
    buf->f_frsize = 1024 * 1024;
    return 0;
}

static int fake_statvfs_slow(const char *path, struct statvfs *buf) {
    (void)path;
    usleep(50000);
    memset(buf, 0, sizeof(*buf));
    buf->f_bavail = 1024;
    buf->f_frsize = 1024 * 1024;
    return 0;
}

static atomic_uint_fast64_t g_fake_time_ms = 0;

static uint64_t fake_time_ms_now(void) {
    return atomic_load(&g_fake_time_ms);
}

static size_t fake_fwrite_short(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr;
    (void)size;
    (void)stream;
    return (nmemb > 0) ? (nmemb - 1) : 0;
}

static int fake_fflush_fail(FILE *stream) {
    (void)stream;
    errno = EIO;
    return EOF;
}

static int fake_pthread_create_fail(pthread_t *thread, const pthread_attr_t *attr,
                                    void *(*start_routine)(void *), void *arg) {
    (void)thread;
    (void)attr;
    (void)start_routine;
    (void)arg;
    return EAGAIN;
}

static int fake_pthread_create_slow(pthread_t *thread, const pthread_attr_t *attr,
                                    void *(*start_routine)(void *), void *arg) {
    usleep(200000);
    return pthread_create(thread, attr, start_routine, arg);
}

static pthread_t g_init_wait_probe_thread = 0;
static bool g_init_wait_probe_started = false;
static volatile int g_init_wait_probe_ready = 0;

static void *init_wait_probe_thread(void *arg) {
    (void)arg;
    g_init_wait_probe_ready = 1;
    dump_pcm_data_write("probe", 5, "init_wait", "same");
    return NULL;
}

static int fake_pthread_create_with_probe(pthread_t *thread, const pthread_attr_t *attr,
                                          void *(*start_routine)(void *), void *arg) {
    if (!g_init_wait_probe_started) {
        g_init_wait_probe_started = true;
        if (pthread_create(&g_init_wait_probe_thread, NULL, init_wait_probe_thread, NULL) != 0) {
            return EAGAIN;
        }
        for (int i = 0; i < 200 && !g_init_wait_probe_ready; ++i) {
            usleep(1000);
        }
    }
    usleep(200000);
    return pthread_create(thread, attr, start_routine, arg);
}

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} writer_bridge_ctx_t;

static void *writer_hold_bridge_thread(void *arg);

static int fake_pthread_create_hold_writer(pthread_t *thread, const pthread_attr_t *attr,
                                           void *(*start_routine)(void *), void *arg) {
    writer_bridge_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return EAGAIN;
    }

    ctx->start_routine = start_routine;
    ctx->arg = arg;
    return pthread_create(thread, attr, writer_hold_bridge_thread, ctx);
}

static int g_logged_messages = 0;

static void counting_log_fn(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
    g_logged_messages++;
}

typedef enum {
    FAIL_KIND_NONE = 0,
    FAIL_KIND_MALLOC,
    FAIL_KIND_CALLOC,
    FAIL_KIND_REALLOC,
    FAIL_KIND_QUEUE_POP,
    FAIL_KIND_PTHREAD_MUTEX_INIT,
    FAIL_KIND_PTHREAD_CONDATTR_INIT,
    FAIL_KIND_PTHREAD_CONDATTR_SETCLOCK,
    FAIL_KIND_PTHREAD_COND_INIT,
} fail_kind_t;

static fail_kind_t g_fail_kind = FAIL_KIND_NONE;
static int g_fail_calls_before_failure = -1;
static atomic_int g_queue_pop_fail_seen = 0;
static atomic_int g_writer_hold_active = 0;
static atomic_int g_writer_hold_ready = 0;
static atomic_int g_writer_hold_release = 0;
static atomic_int g_cond_timedwait_calls = 0;
static atomic_int g_count_dir_ops = 0;
static atomic_int g_dir_ops_on_caller = 0;
static pthread_t g_caller_thread;

static void *writer_hold_bridge_thread(void *arg) {
    writer_bridge_ctx_t *ctx = (writer_bridge_ctx_t *)arg;

    atomic_store(&g_writer_hold_ready, 1);
    while (atomic_load(&g_writer_hold_release) == 0) {
        usleep(1000);
    }

    void *ret = ctx->start_routine(ctx->arg);
    free(ctx);
    return ret;
}

static void set_fail_kind(fail_kind_t kind, int calls_before_failure) {
    g_fail_kind = kind;
    g_fail_calls_before_failure = calls_before_failure;
}

static void clear_fail_kind(void) {
    g_fail_kind = FAIL_KIND_NONE;
    g_fail_calls_before_failure = -1;
}

static bool should_fail(fail_kind_t kind) {
    if (g_fail_kind != kind) {
        return false;
    }

    if (g_fail_calls_before_failure > 0) {
        g_fail_calls_before_failure--;
        return false;
    }

    clear_fail_kind();
    return true;
}

void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
int __real_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int __real_pthread_condattr_init(pthread_condattr_t *attr);
int __real_pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);
int __real_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int __real_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *abstime);
int __real_queue_pop(queue_t *q, data_block_t *block);

void *__wrap_malloc(size_t size) {
    if (should_fail(FAIL_KIND_MALLOC)) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    if (should_fail(FAIL_KIND_CALLOC)) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    if (should_fail(FAIL_KIND_REALLOC)) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_realloc(ptr, size);
}

int __wrap_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    if (should_fail(FAIL_KIND_PTHREAD_MUTEX_INIT)) {
        return EAGAIN;
    }
    return __real_pthread_mutex_init(mutex, attr);
}

int __wrap_pthread_condattr_init(pthread_condattr_t *attr) {
    if (should_fail(FAIL_KIND_PTHREAD_CONDATTR_INIT)) {
        return EAGAIN;
    }
    return __real_pthread_condattr_init(attr);
}

int __wrap_pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id) {
    if (should_fail(FAIL_KIND_PTHREAD_CONDATTR_SETCLOCK)) {
        return EINVAL;
    }
    return __real_pthread_condattr_setclock(attr, clock_id);
}

int __wrap_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    if (should_fail(FAIL_KIND_PTHREAD_COND_INIT)) {
        return EAGAIN;
    }
    return __real_pthread_cond_init(cond, attr);
}

int __wrap_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *abstime) {
    int rc = 0;

    atomic_fetch_add(&g_cond_timedwait_calls, 1);
    rc = __real_pthread_cond_timedwait(cond, mutex, abstime);

    if (atomic_load(&g_writer_hold_active) != 0 && atomic_load(&g_writer_hold_ready) == 0) {
        atomic_store(&g_writer_hold_ready, 1);
        pthread_mutex_unlock(mutex);
        while (atomic_load(&g_writer_hold_release) == 0) {
            usleep(1000);
        }
        pthread_mutex_lock(mutex);
    }

    return rc;
}

DIR *__real_opendir(const char *name);

DIR *__wrap_opendir(const char *name) {
    if (atomic_load(&g_count_dir_ops) != 0 && pthread_equal(pthread_self(), g_caller_thread)) {
        atomic_store(&g_dir_ops_on_caller, 1);
    }
    return __real_opendir(name);
}

int __wrap_queue_pop(queue_t *q, data_block_t *block) {
    if (should_fail(FAIL_KIND_QUEUE_POP)) {
        atomic_store(&g_queue_pop_fail_seen, 1);
        return -1;
    }
    return __real_queue_pop(q, block);
}

static void reset_test_dir(void) {
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    char path[512] = {0};

    if (g_test_dir_base[0] == '\0') {
        strncpy(g_test_dir_base, "/tmp/audio_dump_test", sizeof(g_test_dir_base) - 1);
        g_test_dir_base[sizeof(g_test_dir_base) - 1] = '\0';
    }

    snprintf(path, sizeof(path), "%s_case_%02d/", g_test_dir_base, g_test_dir_seq++);
    file_mgr_set_root_dir(path);

    mkdir(file_mgr_get_root_dir(), 0755);
    dir = opendir(file_mgr_get_root_dir());
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[512];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s%s", file_mgr_get_root_dir(), entry->d_name);
        remove(path);
    }

    closedir(dir);
}

static int count_pcm_files(void) {
    DIR *dir = opendir(file_mgr_get_root_dir());
    struct dirent *entry = NULL;
    int count = 0;

    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".pcm") != NULL) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

static int find_pcm_files(char paths[][512], int max_paths) {
    DIR *dir = opendir(file_mgr_get_root_dir());
    struct dirent *entry = NULL;
    int count = 0;

    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL && count < max_paths) {
        if (strstr(entry->d_name, ".pcm") == NULL) {
            continue;
        }
        snprintf(paths[count], 512, "%s%s", file_mgr_get_root_dir(), entry->d_name);
        count++;
    }

    closedir(dir);
    return count;
}

static int read_file(const char *path, char *buf, size_t size) {
    FILE *fp = fopen(path, "rb");
    size_t n = 0;

    if (fp == NULL) {
        return -1;
    }

    n = fread(buf, 1, size, fp);
    fclose(fp);
    return (int)n;
}

static int file_size(const char *path, uint64_t *size_out) {
    struct stat st = {0};
    if (stat(path, &st) != 0) {
        return -1;
    }
    *size_out = (uint64_t)st.st_size;
    return 0;
}

static void touch_old_file(const char *path);

static bool path_exists(const char *path) {
    struct stat st = {0};
    return path != NULL && stat(path, &st) == 0;
}

static void stop_and_wait(const char *key1, const char *key2) {
    dump_pcm_data_stop(key1, key2);
    usleep(120000);
}

static bool wait_for_queue_pop_fail_seen(void) {
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(&g_queue_pop_fail_seen) != 0) {
            return true;
        }
        usleep(1000);
    }

    return atomic_load(&g_queue_pop_fail_seen) != 0;
}

static bool wait_for_writer_hold_ready(void) {
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(&g_writer_hold_ready) != 0) {
            return true;
        }
        usleep(1000);
    }

    return atomic_load(&g_writer_hold_ready) != 0;
}

static bool wait_for_queue_empty(void) {
    dump_status_t status = {0};

    for (int i = 0; i < 2000; ++i) {
        if (dump_pcm_data_get_status(&status) == 0 && status.queue_pending_blocks == 0) {
            return true;
        }
        usleep(1000);
    }

    return dump_pcm_data_get_status(&status) == 0 && status.queue_pending_blocks == 0;
}

static void test_string_key_write_and_escape(void) {
    const char *name = "string key write and escaping";
    char files[4][512] = {{0}};
    char content[16] = {0};

    reset_test_dir();
    dump_pcm_data_write("abc", 3, "out/primary", "sess.one");
    dump_pcm_data_write("1234", 4, "out/primary", "sess.one");
    usleep(120000);
    stop_and_wait("out/primary", "sess.one");

    if (find_pcm_files(files, 4) != 1) {
        fail(name, "expected one file");
        return;
    }
    if (strstr(files[0], "out_primary_sess_one_") == NULL) {
        fail(name, "escaped filename missing");
        return;
    }
    if (read_file(files[0], content, sizeof(content)) != 7 || memcmp(content, "abc1234", 7) != 0) {
        fail(name, "file content mismatch");
        return;
    }

    pass(name);
}

static void test_key_validation(void) {
    const char *name = "key validation";
    char too_long[80];
    dump_status_t status = {0};

    memset(too_long, 'k', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    reset_test_dir();

    dump_pcm_data_write("payload", 7, NULL, "ok");
    dump_pcm_data_write("payload", 7, "", "ok");
    dump_pcm_data_write("payload", 7, too_long, "ok");
    dump_pcm_data_write("payload", 7, "ok", NULL);
    dump_pcm_data_write("payload", 7, "ok", "");
    dump_pcm_data_write(NULL, 4, "ok", "ok");
    dump_pcm_data_write("payload", 0, "ok", "ok");
    usleep(80000);

    if (count_pcm_files() != 0 || dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        fail(name, "invalid writes mutated state");
        return;
    }

    pass(name);
}

static void test_argument_guard_coverage(void) {
    const char *name = "argument guard coverage";
    queue_t queue;
    data_block_t block = {0};
    data_block_t out = {0};
    session_manager_t session_mgr;
    file_manager_t file_mgr;
    char keybuf[DUMP_PCM_KEY_MAX_LEN + 1] = {0};
    char smallbuf[16] = {0};
    bool is_new = false;
    int actual = -1;

    dump_key_copy(NULL, "copy-null-dst");
    dump_key_copy(keybuf, NULL);
    if (keybuf[0] != '\0') {
        fail(name, "null source should clear destination");
        return;
    }
    dump_key_escape(NULL, keybuf, sizeof(keybuf));
    dump_key_escape(keybuf, NULL, sizeof(keybuf));
    if (keybuf[0] != '\0') {
        fail(name, "null escape source should clear destination");
        return;
    }
    dump_key_escape(smallbuf, "x y", 0);

    if (dump_key_is_valid(NULL) != 0) {
        fail(name, "NULL key should be invalid");
        return;
    }

    dump_pcm_data_stop(NULL, "stop");
    dump_pcm_data_stop("stop", NULL);

    queue_init(NULL, 1, 1);
    queue_init(&queue, 1, 1);
    block.buffer = (char *)"q";
    block.buffer_size = 1;
    if (queue_push(NULL, &block) != -1 || queue_push(&queue, NULL) != -1 || queue_pop(NULL, &out) != -1 ||
        queue_pop(&queue, NULL) != -1 || queue_is_full(NULL) != 0 || queue_count(NULL) != 0 ||
        queue_bytes(NULL) != 0) {
        queue_destroy(&queue);
        fail(name, "queue guards failed");
        return;
    }
    queue_destroy(NULL);
    queue_destroy(&queue);

    session_mgr_init(NULL);
    session_mgr_init(&session_mgr);
    if (session_find(NULL, "a", "b") != NULL || session_find(&session_mgr, NULL, "b") != NULL ||
        session_find_by_generation(NULL, "a", "b", 1) != NULL ||
        session_find_by_generation(&session_mgr, "a", NULL, 1) != NULL ||
        session_create(NULL, "a", "b", 1) != NULL || session_create(&session_mgr, NULL, "b", 1) != NULL ||
        session_get_or_create(NULL, "a", "b") != NULL || session_live_count(NULL) != 0 ||
        session_count(NULL) != 0) {
        session_mgr_destroy(&session_mgr);
        fail(name, "session guards failed");
        return;
    }
    {
        session_info_t *session = session_create(&session_mgr, "stay", "open", 1);
        if (session == NULL) {
            session_mgr_destroy(&session_mgr);
            fail(name, "failed to create session for stopping branch");
            return;
        }
        session->accepting = false;
        session->state = SESSION_STOPPING;
        if (session_find_accepting(&session_mgr, "stay", "open") != NULL ||
            session_find(&session_mgr, "stay", "open") != session ||
            session_find_by_generation(&session_mgr, "stay", "open", 999) != NULL ||
            session_find_by_generation(&session_mgr, "missing", "open", 1) != NULL ||
            session_live_count(&session_mgr) != 1) {
            session_mgr_destroy(&session_mgr);
            fail(name, "stopping session branch failed");
            return;
        }
    }
    session_destroy(NULL, NULL);
    session_mgr_destroy(NULL);
    session_mgr_destroy(&session_mgr);

    {
        char long_dir[300] = {0};
        memset(long_dir, 'd', sizeof(long_dir) - 1);
        file_mgr_set_root_dir(NULL);
        file_mgr_set_root_dir(long_dir);
    }

    if (file_mgr_init(NULL) != -1) {
        fail(name, "NULL file manager should fail init");
        return;
    }
    if (file_mgr_get_handle(NULL, "a", "b", 1, 0, keybuf, sizeof(keybuf), &is_new) != NULL ||
        file_mgr_get_handle(&file_mgr, NULL, "b", 1, 0, keybuf, sizeof(keybuf), &is_new) != NULL ||
        file_mgr_get_handle(&file_mgr, "a", NULL, 1, 0, keybuf, sizeof(keybuf), &is_new) != NULL ||
        file_mgr_get_handle(&file_mgr, "a", "b", 1, 0, NULL, sizeof(keybuf), &is_new) != NULL ||
        file_mgr_get_handle(&file_mgr, "a", "b", 1, 0, keybuf, 0, &is_new) != NULL ||
        file_mgr_get_handle(&file_mgr, "a", "b", 1, 0, keybuf, sizeof(keybuf), NULL) != NULL) {
        fail(name, "file handle guards failed");
        return;
    }
    if (file_mgr_write(NULL, NULL, "x", 1) != -1 || file_mgr_write(&file_mgr, NULL, "x", 1) != -1 ||
        file_mgr_write(&file_mgr, (FILE *)0x1, NULL, 1) != -1 || file_mgr_write(&file_mgr, (FILE *)0x1, "x", 0) != -1 ||
        file_mgr_close(NULL, NULL) != -1 || file_mgr_close(&file_mgr, NULL) != -1 ||
        file_mgr_is_tracking(NULL, NULL) != false || file_mgr_is_tracking(&file_mgr, NULL) != false) {
        fail(name, "file manager guards failed");
        return;
    }
    {
        char foreign_path[256] = {0};
        FILE *foreign = NULL;

        snprintf(foreign_path, sizeof(foreign_path), "%sforeign_check.tmp", file_mgr_get_root_dir());
        foreign = fopen(foreign_path, "wb");
        if (foreign == NULL) {
            fail(name, "failed to open foreign handle for tracking check");
            return;
        }
        if (file_mgr_is_tracking(&file_mgr, foreign)) {
            fclose(foreign);
            fail(name, "untracked handle should not be reported as tracked");
            return;
        }
        fclose(foreign);
        remove(foreign_path);
    }
    file_mgr_destroy(NULL);

    if (dump_pcm_data_get_active_sessions(NULL, 1, &actual) != -1 ||
        dump_pcm_data_get_active_sessions(NULL, 0, &actual) != 0 || actual != 0) {
        fail(name, "active session guards failed");
        return;
    }

    pass(name);
}

static void test_storage_cache_interval_hits_cached_path(void) {
    const char *name = "storage cache interval hits cached path";
    dump_status_t status = {0};

    reset_test_dir();
    dump_pcm_data_write("cache", 5, "interval", "path");
    dump_pcm_data_write("cache2", 6, "interval", "path");
    usleep(120000);
    stop_and_wait("interval", "path");

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        fail(name, "cached storage path did not preserve normal state");
        return;
    }

    pass(name);
}

typedef struct {
    pthread_barrier_t *barrier;
    const char *key1;
    const char *key2;
    const char *payload;
} concurrent_write_args_t;

static void *concurrent_dump_write(void *arg) {
    const concurrent_write_args_t *write_args = (const concurrent_write_args_t *)arg;

    pthread_barrier_wait(write_args->barrier);
    dump_pcm_data_write(write_args->payload, (uint32_t)strlen(write_args->payload), write_args->key1,
                        write_args->key2);
    return NULL;
}

static void test_storage_cache_hits_cached_under_lock(void) {
    const char *name = "storage cache hits cached under lock";
    pthread_barrier_t barrier;
    pthread_t threads[2] = {0};
    concurrent_write_args_t args[2] = {{0}};
    dump_status_t status = {0};

    reset_test_dir();
    dump_pcm_data_write("warm", 4, "cache_lock", "same");
    usleep(120000);
    stop_and_wait("cache_lock", "same");
    usleep(1100000);
    dump_pcm_set_statvfs_fn_for_test(fake_statvfs_slow);
    if (pthread_barrier_init(&barrier, NULL, 3) != 0) {
        dump_pcm_set_statvfs_fn_for_test(NULL);
        fail(name, "failed to initialize barrier");
        return;
    }

    args[0].barrier = &barrier;
    args[0].key1 = "cache_lock";
    args[0].key2 = "same";
    args[0].payload = "A";
    args[1].barrier = &barrier;
    args[1].key1 = "cache_lock";
    args[1].key2 = "same";
    args[1].payload = "B";

    if (pthread_create(&threads[0], NULL, concurrent_dump_write, &args[0]) != 0 ||
        pthread_create(&threads[1], NULL, concurrent_dump_write, &args[1]) != 0) {
        pthread_barrier_destroy(&barrier);
        dump_pcm_set_statvfs_fn_for_test(NULL);
        fail(name, "failed to start concurrent writers");
        return;
    }

    pthread_barrier_wait(&barrier);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    pthread_barrier_destroy(&barrier);
    dump_pcm_set_statvfs_fn_for_test(NULL);
    usleep(120000);
    stop_and_wait("cache_lock", "same");

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        fail(name, "cached storage path under lock did not settle cleanly");
        return;
    }

    pass(name);
}

static void test_session_separation_and_query(void) {
    const char *name = "session separation and query";
    dump_session_info_t sessions[8] = {0};
    int count = 0;

    reset_test_dir();
    dump_pcm_data_write("xy", 2, "stream_a", "one");
    dump_pcm_data_write("xy", 2, "stream_a", "two");
    dump_pcm_data_write("xy", 2, "stream_b", "one");
    usleep(100000);

    if (dump_pcm_data_get_active_sessions(sessions, 8, &count) != 0 || count != 3) {
        fail(name, "expected three sessions");
        return;
    }

    stop_and_wait("stream_a", "one");
    stop_and_wait("stream_a", "two");
    stop_and_wait("stream_b", "one");
    pass(name);
}

static void test_stop_then_new_session_creates_new_file(void) {
    const char *name = "stop then new session creates new file";
    char files[4][512] = {{0}};
    char content[16] = {0};
    bool saw_first = false;
    bool saw_second = false;
    int file_count = 0;

    reset_test_dir();
    dump_pcm_data_write("first", 5, "stream_stop", "same");
    usleep(80000);
    stop_and_wait("stream_stop", "same");
    usleep(20000);
    dump_pcm_data_write("second", 6, "stream_stop", "same");
    usleep(80000);
    stop_and_wait("stream_stop", "same");

    file_count = find_pcm_files(files, 4);
    if (file_count != 2) {
        fail(name, "expected two files");
        return;
    }

    for (int i = 0; i < file_count; ++i) {
        int read_count = read_file(files[i], content, sizeof(content));
        if (read_count == 5 && memcmp(content, "first", 5) == 0) {
            saw_first = true;
        } else if (read_count == 6 && memcmp(content, "second", 6) == 0) {
            saw_second = true;
        }
    }

    if (!saw_first || !saw_second) {
        fail(name, "expected separate contents in distinct files");
        return;
    }

    pass(name);
}

static void test_write_gap_exactly_two_seconds_keeps_same_file(void) {
    const char *name = "write gap exactly two seconds keeps same file";
    char files[4][512] = {{0}};
    char content[16] = {0};
    bool seam_installed = false;
    int file_count = 0;
    int read_count = 0;

    reset_test_dir();
    dump_pcm_set_time_ms_fn_for_test(fake_time_ms_now);
    seam_installed = true;

    atomic_store(&g_fake_time_ms, 1000);
    dump_pcm_data_write("aa", 2, "gap", "boundary");
    usleep(80000);

    atomic_store(&g_fake_time_ms, 3000);
    dump_pcm_data_write("bb", 2, "gap", "boundary");
    usleep(80000);

    stop_and_wait("gap", "boundary");
    file_count = find_pcm_files(files, 4);
    if (file_count != 1) {
        fail(name, "expected one file at exact 2-second boundary");
        goto cleanup;
    }
    read_count = read_file(files[0], content, sizeof(content));
    if (read_count != 4 || memcmp(content, "aabb", 4) != 0) {
        fail(name, "exact boundary write did not stay in one file");
        goto cleanup;
    }

    pass(name);

cleanup:
    if (seam_installed) {
        dump_pcm_set_time_ms_fn_for_test(NULL);
    }
    atomic_store(&g_fake_time_ms, 0);
}

static void test_write_gap_over_two_seconds_creates_new_file(void) {
    const char *name = "write gap over two seconds creates new file";
    char files[4][512] = {{0}};
    char content[16] = {0};
    bool seam_installed = false;
    bool saw_aa = false;
    bool saw_bb = false;
    int file_count = 0;

    reset_test_dir();
    dump_pcm_set_time_ms_fn_for_test(fake_time_ms_now);
    seam_installed = true;

    atomic_store(&g_fake_time_ms, 1000);
    dump_pcm_data_write("aa", 2, "gap", "rollover");
    usleep(80000);

    atomic_store(&g_fake_time_ms, 3001);
    dump_pcm_data_write("bb", 2, "gap", "rollover");
    usleep(80000);

    stop_and_wait("gap", "rollover");
    file_count = find_pcm_files(files, 4);
    if (file_count != 2) {
        fail(name, "expected two files after rollover gap");
        goto cleanup;
    }

    for (int i = 0; i < file_count; ++i) {
        int read_count = read_file(files[i], content, sizeof(content));
        if (read_count == 2 && memcmp(content, "aa", 2) == 0) {
            saw_aa = true;
        } else if (read_count == 2 && memcmp(content, "bb", 2) == 0) {
            saw_bb = true;
        }
    }

    if (!saw_aa || !saw_bb) {
        fail(name, "rollover gap did not split content across two files");
        goto cleanup;
    }

    pass(name);

cleanup:
    if (seam_installed) {
        dump_pcm_set_time_ms_fn_for_test(NULL);
    }
    atomic_store(&g_fake_time_ms, 0);
}

static void test_low_storage_latch_requires_reenable(void) {
    const char *name = "low storage latch requires reenable";
    dump_status_t status = {0};

    reset_test_dir();
    setenv("AUDIO_DUMP_TEST_AVAILABLE_MB", "100", 1);
    dump_pcm_data_write("low", 3, "low_key", "one");
    usleep(60000);
    dump_pcm_data_get_status(&status);
    if (!status.low_storage_triggered) {
        fail(name, "low storage latch did not trigger");
        return;
    }

    setenv("AUDIO_DUMP_TEST_AVAILABLE_MB", "1000", 1);
    dump_pcm_data_write("low", 3, "low_key", "two");
    usleep(60000);
    dump_pcm_data_get_status(&status);
    if (!status.low_storage_triggered || status.active_session_count != 0) {
        fail(name, "latch cleared without re-enable");
        return;
    }

    unsetenv("AUDIO_DUMP_ENABLED");
    dump_pcm_data_write("low", 3, "low_key", "three");
    usleep(30000);
    setenv("AUDIO_DUMP_ENABLED", "1", 1);
    dump_pcm_data_write("low", 3, "low_key", "three");
    usleep(80000);
    dump_pcm_data_get_status(&status);
    unsetenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
    stop_and_wait("low_key", "three");

    if (status.low_storage_triggered || status.active_session_count == 0 || count_pcm_files() != 1) {
        fail(name, "expected latch clear after off-on toggle");
        return;
    }

    pass(name);
}

static void test_filename_hash_disambiguates_escaped_collisions(void) {
    const char *name = "filename hash disambiguates escaped collisions";
    char files[8][512] = {{0}};

    reset_test_dir();
    dump_pcm_data_write("k", 1, "port/1", "sess:1");
    dump_pcm_data_write("k", 1, "port_1", "sess/1");
    usleep(100000);
    stop_and_wait("port/1", "sess:1");
    stop_and_wait("port_1", "sess/1");

    if (find_pcm_files(files, 8) != 2) {
        fail(name, "expected two files");
        return;
    }
    if (strcmp(files[0], files[1]) == 0 || strstr(files[0], "port_1_sess_1_") == NULL ||
        strstr(files[1], "port_1_sess_1_") == NULL) {
        fail(name, "hash did not disambiguate escaped names");
        return;
    }

    pass(name);
}

static void test_same_timestamp_rollovers_get_unique_files(void) {
    const char *name = "same timestamp rollovers get unique files";
    char files[8][512] = {{0}};
    int file_count = 0;

    reset_test_dir();
    file_mgr_set_timestamp_override_for_test("20260421_010203_004");
    dump_pcm_set_file_size_limit_for_test(1);
    dump_pcm_data_write("A", 1, "rollover", "same_ts");
    dump_pcm_data_write("B", 1, "rollover", "same_ts");
    dump_pcm_data_write("C", 1, "rollover", "same_ts");
    usleep(150000);
    stop_and_wait("rollover", "same_ts");
    dump_pcm_set_file_size_limit_for_test(0);
    file_mgr_set_timestamp_override_for_test(NULL);

    file_count = find_pcm_files(files, 8);
    if (file_count != 3) {
        printf("Observed file count: %d\n", file_count);
        fail(name, "expected three rollover files under fixed timestamp");
        return;
    }
    if (strcmp(files[0], files[1]) == 0 || strcmp(files[0], files[2]) == 0 || strcmp(files[1], files[2]) == 0) {
        fail(name, "rollover file paths collided");
        return;
    }
    if (strstr(files[0], "20260421_010203_004.pcm") == NULL ||
        strstr(files[1], "20260421_010203_004.pcm") == NULL ||
        strstr(files[2], "20260421_010203_004.pcm") == NULL) {
        fail(name, "fixed timestamp hook did not apply");
        return;
    }

    pass(name);
}

static void test_statvfs_failure_blocks_new_session(void) {
    const char *name = "statvfs failure blocks new session";
    dump_status_t before = {0};
    dump_status_t after = {0};

    reset_test_dir();
    unsetenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
    usleep(1100000);
    dump_pcm_data_get_status(&before);
    dump_pcm_set_statvfs_fn_for_test(fake_statvfs_fail);
    dump_pcm_data_write("s", 1, "statvfs", "fail");
    usleep(60000);
    dump_pcm_set_statvfs_fn_for_test(NULL);
    dump_pcm_data_get_status(&after);

    if (after.active_session_count != before.active_session_count) {
        fail(name, "statvfs failure admitted a session");
        return;
    }

    pass(name);
}

static void test_statvfs_low_storage_blocks_new_session(void) {
    const char *name = "statvfs low storage blocks new session";
    dump_status_t status = {0};

    reset_test_dir();
    dump_pcm_set_statvfs_fn_for_test(fake_statvfs_low);
    dump_pcm_data_write("s", 1, "statvfs", "low");
    usleep(60000);
    dump_pcm_set_statvfs_fn_for_test(NULL);
    if (dump_pcm_data_get_status(&status) != 0 || status.low_storage_triggered != 1 ||
        status.active_session_count != 0) {
        fail(name, "low storage branch did not latch correctly");
        return;
    }

    pass(name);
}

static void test_statvfs_large_storage_reports_full_value(void) {
    const char *name = "statvfs large storage reports full value";
    dump_status_t status = {0};

    reset_test_dir();
    unsetenv("AUDIO_DUMP_ENABLED");
    dump_pcm_data_write("s", 1, "statvfs", "reset");
    usleep(30000);
    setenv("AUDIO_DUMP_ENABLED", "1", 1);
    unsetenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
    usleep(1100000);
    dump_pcm_set_statvfs_fn_for_test(fake_statvfs_large);
    dump_pcm_data_write("s", 1, "statvfs", "large");
    usleep(60000);
    dump_pcm_set_statvfs_fn_for_test(NULL);

    if (dump_pcm_data_get_status(&status) != 0 || status.available_storage_mb != 33792 ||
        status.active_session_count != 1) {
        stop_and_wait("statvfs", "large");
        fail(name, "expected full 64-bit storage value to be preserved");
        return;
    }

    stop_and_wait("statvfs", "large");
    pass(name);
}

static void test_queue_direct(void) {
    const char *name = "queue direct";
    queue_t queue;
    data_block_t in = {0};
    data_block_t out = {0};
    char payload[] = "queue";

    queue_init(&queue, 1, 8);
    dump_key_copy(in.key1, "q1");
    dump_key_copy(in.key2, "q2");
    in.buffer = payload;
    in.buffer_size = (uint32_t)strlen(payload);
    in.generation = 3;

    if (queue_push(&queue, &in) != 0 || queue_pop(&queue, &out) != 0 || strcmp(out.key1, "q1") != 0 ||
        strcmp(out.key2, "q2") != 0 || out.generation != 3 || memcmp(out.buffer, payload, 5) != 0) {
        free(out.buffer);
        queue_destroy(&queue);
        fail(name, "queue push or pop mismatch");
        return;
    }
    free(out.buffer);

    if (queue_push(&queue, &in) != 0 || queue_push(&queue, &in) == 0) {
        queue_destroy(&queue);
        fail(name, "queue capacity guard failed");
        return;
    }

    queue_destroy(&queue);
    pass(name);
}

static void test_queue_empty_pop_and_byte_limit(void) {
    const char *name = "queue empty pop and byte limit";
    queue_t queue;
    data_block_t in = {0};
    data_block_t out = {0};
    char payload[] = "abcd";

    queue_init(&queue, 2, 4);
    if (queue_pop(&queue, &out) == 0) {
        queue_destroy(&queue);
        fail(name, "empty queue pop should fail");
        return;
    }

    dump_key_copy(in.key1, "q1");
    dump_key_copy(in.key2, "q2");
    in.buffer = payload;
    in.buffer_size = 4;

    if (queue_push(&queue, &in) != 0 || queue_is_full(&queue) == 0 || queue_push(&queue, &in) == 0 ||
        queue_count(&queue) != 1 || queue_bytes(&queue) != 4 || queue_pop(&queue, &out) != 0 ||
        memcmp(out.buffer, payload, 4) != 0 || queue_is_empty(&queue) == 0) {
        free(out.buffer);
        queue_destroy(&queue);
        fail(name, "queue byte limit or empty pop behavior mismatch");
        return;
    }

    free(out.buffer);
    queue_destroy(&queue);
    pass(name);
}

static void test_session_direct(void) {
    const char *name = "session direct";
    session_manager_t mgr;
    session_info_t *first = NULL;
    session_info_t *second = NULL;

    session_mgr_init(&mgr);
    first = session_create(&mgr, "port/1", "sess:1", 1);
    second = session_create(&mgr, "port_1", "sess/1", 2);
    if (first == NULL || second == NULL || session_find(&mgr, "port/1", "sess:1") != first ||
        session_find_by_generation(&mgr, "port_1", "sess/1", 2) != second || session_live_count(&mgr) != 2) {
        session_mgr_destroy(&mgr);
        fail(name, "session lookup mismatch");
        return;
    }

    session_destroy(&mgr, first);
    session_destroy(&mgr, second);
    session_mgr_destroy(&mgr);
    pass(name);
}

static void test_session_highest_generation_lookup(void) {
    const char *name = "session highest generation lookup";
    session_manager_t mgr;
    session_info_t *first = NULL;
    session_info_t *second = NULL;

    session_mgr_init(&mgr);
    first = session_create(&mgr, "same", "key", 1);
    second = session_create(&mgr, "same", "key", 2);
    if (first == NULL || second == NULL || session_find(&mgr, "same", "key") != second ||
        session_find_by_generation(&mgr, "same", "key", 1) != first ||
        session_find_by_generation(&mgr, "same", "key", 2) != second) {
        session_mgr_destroy(&mgr);
        fail(name, "highest generation should win");
        return;
    }

    session_mgr_destroy(&mgr);
    pass(name);
}

static void test_queue_allocation_failures(void) {
    const char *name = "queue allocation failures";
    queue_t queue;
    data_block_t block = {0};
    char payload[] = "queue";

    queue_init(&queue, 2, 16);
    dump_key_copy(block.key1, "q1");
    dump_key_copy(block.key2, "q2");
    block.buffer = payload;
    block.buffer_size = (uint32_t)strlen(payload);

    set_fail_kind(FAIL_KIND_MALLOC, 0);
    if (queue_push(&queue, &block) != -1 || queue_count(&queue) != 0) {
        clear_fail_kind();
        queue_destroy(&queue);
        fail(name, "node allocation failure was not rejected");
        return;
    }

    set_fail_kind(FAIL_KIND_MALLOC, 1);
    if (queue_push(&queue, &block) != -1 || queue_count(&queue) != 0) {
        clear_fail_kind();
        queue_destroy(&queue);
        fail(name, "buffer allocation failure was not rejected");
        return;
    }

    clear_fail_kind();
    queue_destroy(&queue);
    pass(name);
}

static void test_session_allocation_failures(void) {
    const char *name = "session allocation failures";
    session_manager_t mgr;
    session_info_t dummy[MAX_SESSIONS] = {{0}};
    session_info_t *session = NULL;

    session_mgr_init(&mgr);

    set_fail_kind(FAIL_KIND_CALLOC, 0);
    session = session_create(&mgr, "alloc", "fail", 1);
    if (session != NULL || session_count(&mgr) != 0) {
        clear_fail_kind();
        session_mgr_destroy(&mgr);
        fail(name, "calloc failure was not rejected");
        return;
    }

    clear_fail_kind();

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        mgr.sessions[i] = &dummy[i];
    }
    mgr.count = 0;
    if (session_create(&mgr, "full", "table", 2) != NULL) {
        fail(name, "full session table should reject with NULL return");
        return;
    }

    pass(name);
}

static int run_isolated_public_write_allocation_failures(void) {
    dump_status_t status = {0};

    reset_test_dir();
    set_fail_kind(FAIL_KIND_CALLOC, 0);
    dump_pcm_data_write("x", 1, "pub_alloc", "session");
    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        clear_fail_kind();
        return 1;
    }

    reset_test_dir();
    set_fail_kind(FAIL_KIND_MALLOC, 1);
    dump_pcm_data_write("x", 1, "pub_alloc", "queue");
    usleep(80000);
    clear_fail_kind();
    dump_pcm_data_stop("pub_alloc", "queue");
    usleep(60000);
    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        return 1;
    }

    return 0;
}

static void test_public_write_allocation_failures(void) {
    const char *name = "public write allocation failures";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_PUBLIC_WRITE_ALLOC_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated public write allocation failure test failed");
        return;
    }

    pass(name);
}

static int run_isolated_writer_queue_pop_failure(void) {
    dump_status_t status = {0};

    reset_test_dir();
    atomic_store(&g_queue_pop_fail_seen, 0);
    set_fail_kind(FAIL_KIND_QUEUE_POP, 0);
    dump_pcm_data_write("x", 1, "pop_fail", "one");
    if (!wait_for_queue_pop_fail_seen()) {
        clear_fail_kind();
        return 1;
    }
    dump_pcm_data_stop("pop_fail", "one");
    clear_fail_kind();
    usleep(200000);

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        return 1;
    }

    return 0;
}

static int run_isolated_queue_full_write_rejection(void) {
    dump_status_t status = {0};

    reset_test_dir();
    dump_pcm_set_pthread_create_fn_for_test(fake_pthread_create_hold_writer);
    atomic_store(&g_writer_hold_ready, 0);
    atomic_store(&g_writer_hold_release, 0);
    dump_pcm_data_write("A", 1, "queue_full", "one");
    if (!wait_for_writer_hold_ready()) {
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        atomic_store(&g_writer_hold_release, 1);
        return 1;
    }

    dump_pcm_data_write("B", 1, "queue_full", "one");
    dump_pcm_data_write("C", 1, "queue_full", "one");
    dump_pcm_data_write("D", 1, "queue_full", "one");
    dump_pcm_data_write("E", 1, "queue_full", "one");
    dump_pcm_data_write("F", 1, "queue_full", "one");
    dump_pcm_data_write("G", 1, "queue_full", "one");
    dump_pcm_data_write("H", 1, "queue_full", "one");
    dump_pcm_data_write("I", 1, "queue_full", "one");

    if (dump_pcm_data_get_status(&status) != 0 || status.queue_pending_blocks != MAX_QUEUE_BLOCKS ||
        status.active_session_count != 1) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 1;
    }

    atomic_store(&g_writer_hold_release, 1);
    dump_pcm_set_pthread_create_fn_for_test(NULL);
    usleep(200000);
    stop_and_wait("queue_full", "one");

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        return 1;
    }

    return 0;
}

static int run_isolated_queue_full_failed_enqueue_clock_test(void) {
    char files[8][512] = {{0}};
    int file_count = 0;

    reset_test_dir();
    dump_pcm_set_time_ms_fn_for_test(fake_time_ms_now);
    dump_pcm_set_pthread_create_fn_for_test(fake_pthread_create_hold_writer);
    atomic_store(&g_writer_hold_ready, 0);
    atomic_store(&g_writer_hold_release, 0);

    atomic_store(&g_fake_time_ms, 1000);
    dump_pcm_data_write("A", 1, "queue_clock", "one");
    if (!wait_for_writer_hold_ready()) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        dump_pcm_set_time_ms_fn_for_test(NULL);
        return 1;
    }

    for (int i = 0; i < MAX_QUEUE_BLOCKS - 1; ++i) {
        dump_pcm_data_write("B", 1, "queue_clock", "one");
    }

    atomic_store(&g_fake_time_ms, 3501);
    dump_pcm_data_write("Z", 1, "queue_clock", "one");

    atomic_store(&g_writer_hold_release, 1);
    dump_pcm_set_pthread_create_fn_for_test(NULL);
    if (!wait_for_queue_empty()) {
        dump_pcm_set_time_ms_fn_for_test(NULL);
        return 2;
    }

    atomic_store(&g_fake_time_ms, 4501);
    dump_pcm_data_write("C", 1, "queue_clock", "one");
    usleep(120000);
    stop_and_wait("queue_clock", "one");

    file_count = find_pcm_files(files, 8);
    dump_pcm_set_time_ms_fn_for_test(NULL);
    atomic_store(&g_fake_time_ms, 0);
    if (file_count != 2) {
        return 3;
    }

    return 0;
}

static void test_writer_queue_pop_failure(void) {
    const char *name = "writer queue pop failure";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_QUEUE_POP_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated queue_pop failure test failed");
        return;
    }

    pass(name);
}

static void test_writer_queue_full_write_rejection(void) {
    const char *name = "writer queue full write rejection";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_QUEUE_FULL_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated queue_full write rejection test failed");
        return;
    }

    pass(name);
}

static void test_writer_queue_full_failed_enqueue_keeps_rollover_clock(void) {
    const char *name = "writer queue full failed enqueue keeps rollover clock";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_QUEUE_FULL_CLOCK_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated queue_full clock test failed");
        return;
    }

    pass(name);
}

static void test_file_manager_idle_scan_skips_inactive_slots(void) {
    const char *name = "file manager idle scan skips inactive slots";
    file_manager_t mgr;
    bool is_new = false;
    char path_a[256] = {0};
    char path_b[256] = {0};
    FILE *tracked = NULL;
    FILE *next = NULL;

    reset_test_dir();
    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    tracked = file_mgr_get_handle(&mgr, "idle", "a", 1, 0, path_a, sizeof(path_a), &is_new);
    if (tracked == NULL) {
        file_mgr_destroy(&mgr);
        fail(name, "failed to open tracked handle");
        return;
    }

    mgr.count = MAX_OPEN_FILES;
    mgr.files[1].in_use = false;
    sleep(IDLE_TIMEOUT_SEC + 1);

    next = file_mgr_get_handle(&mgr, "idle", "b", 2, 0, path_b, sizeof(path_b), &is_new);
    if (next == NULL || mgr.count != MAX_OPEN_FILES) {
        file_mgr_destroy(&mgr);
        fail(name, "idle scan did not reuse slot cleanly");
        return;
    }

    file_mgr_destroy(&mgr);
    pass(name);
}

static void test_file_manager_cleanup_realloc_failure(void) {
    const char *name = "file manager cleanup realloc failure";
    char path[40][512] = {{0}};
    FILE *fp = NULL;
    int created = 0;

    reset_test_dir();
    for (int i = 0; i < 40; ++i) {
        snprintf(path[i], sizeof(path[i]), "%sold_%02d.pcm", file_mgr_get_root_dir(), i);
        fp = fopen(path[i], "wb");
        if (fp == NULL) {
            break;
        }
        fclose(fp);
        touch_old_file(path[i]);
        created++;
    }

    set_fail_kind(FAIL_KIND_REALLOC, 0);
    if (file_mgr_cleanup(0, 0) != -1) {
        clear_fail_kind();
        for (int i = 0; i < created; ++i) {
            remove(path[i]);
        }
        fail(name, "realloc failure was not rejected");
        return;
    }

    clear_fail_kind();
    for (int i = 0; i < created; ++i) {
        remove(path[i]);
    }
    pass(name);
}

static void test_session_capacity_rejection(void) {
    const char *name = "session capacity rejection";
    session_manager_t mgr;
    char key1[32] = {0};
    char key2[32] = {0};

    session_mgr_init(&mgr);

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        snprintf(key1, sizeof(key1), "cap%u", i);
        snprintf(key2, sizeof(key2), "slot%u", i);
        if (session_create(&mgr, key1, key2, i + 1) == NULL) {
            session_mgr_destroy(&mgr);
            fail(name, "failed to fill session capacity");
            return;
        }
    }

    if (session_count(&mgr) != MAX_SESSIONS || session_create(&mgr, "overflow", "guard", 99) != NULL) {
        session_mgr_destroy(&mgr);
        fail(name, "session capacity guard failed");
        return;
    }

    session_mgr_destroy(&mgr);
    pass(name);
}

static void test_public_write_rejects_when_session_capacity_reached(void) {
    const char *name = "public write rejects when session capacity reached";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_SESSION_CAP_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "expected session cap to stay fixed after overflow write");
        return;
    }
    pass(name);
}

static int run_isolated_stop_while_pending_blocks(void) {
    dump_status_t status = {0};

    dump_pcm_set_pthread_create_fn_for_test(fake_pthread_create_hold_writer);
    atomic_store(&g_writer_hold_ready, 0);
    atomic_store(&g_writer_hold_release, 0);
    reset_test_dir();
    for (int i = 0; i < 128; ++i) {
        dump_pcm_data_write("payload", 7, "pending", "stop");
    }

    if (!wait_for_writer_hold_ready()) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 2;
    }

    dump_pcm_data_stop("pending", "stop");
    if (dump_pcm_data_get_status(&status) != 0) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 3;
    }
    if (status.queue_pending_blocks == 0) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 6;
    }

    atomic_store(&g_writer_hold_release, 1);
    dump_pcm_set_pthread_create_fn_for_test(NULL);
    usleep(200000);

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        return 4;
    }

    return 0;
}

static void test_stop_while_pending_blocks(void) {
    const char *name = "stop while pending blocks";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_STOP_PENDING_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated pending stop test failed");
        return;
    }

    pass(name);
}

static void test_file_manager_direct(void) {
    const char *name = "file manager direct";
    file_manager_t mgr;
    bool is_new = false;
    char path_a[256] = {0};
    char path_b[256] = {0};
    FILE *fp_a = NULL;
    FILE *fp_b = NULL;
    uint64_t size = 0;

    reset_test_dir();
    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    fp_a = file_mgr_get_handle(&mgr, "port/1", "sess:1", 1, 0, path_a, sizeof(path_a), &is_new);
    fp_b = file_mgr_get_handle(&mgr, "port_1", "sess/1", 2, 0, path_b, sizeof(path_b), &is_new);
    if (fp_a == NULL || fp_b == NULL || strcmp(path_a, path_b) == 0) {
        file_mgr_destroy(&mgr);
        fail(name, "file manager paths not unique");
        return;
    }

    if (file_mgr_write(&mgr, fp_a, "abc", 3) != 0 || file_mgr_close(&mgr, fp_a) != 0 ||
        file_size(path_a, &size) != 0 || size != 3) {
        file_mgr_destroy(&mgr);
        fail(name, "file write or close failed");
        return;
    }

    file_mgr_set_fwrite_fn_for_test(fake_fwrite_short);
    if (file_mgr_write(&mgr, fp_b, "abc", 3) == 0) {
        file_mgr_set_fwrite_fn_for_test(NULL);
        file_mgr_destroy(&mgr);
        fail(name, "short write should fail");
        return;
    }
    file_mgr_set_fwrite_fn_for_test(NULL);

    file_mgr_set_fflush_fn_for_test(fake_fflush_fail);
    if (file_mgr_write(&mgr, fp_b, "abc", 3) == 0) {
        file_mgr_set_fflush_fn_for_test(NULL);
        file_mgr_destroy(&mgr);
        fail(name, "flush failure should fail");
        return;
    }
    file_mgr_set_fflush_fn_for_test(NULL);

    file_mgr_close(&mgr, fp_b);
    file_mgr_destroy(&mgr);
    pass(name);
}

static void test_file_manager_reuses_tracked_handle(void) {
    const char *name = "file manager reuses tracked handle";
    file_manager_t mgr;
    bool is_new = false;
    char path[256] = {0};
    FILE *first = NULL;
    FILE *second = NULL;

    reset_test_dir();
    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    first = file_mgr_get_handle(&mgr, "reuse", "same", 1, 0, path, sizeof(path), &is_new);
    second = file_mgr_get_handle(&mgr, "reuse", "same", 1, 0, path, sizeof(path), &is_new);
    if (first == NULL || second != first || mgr.count != 1) {
        file_mgr_destroy(&mgr);
        fail(name, "tracked handle was not reused");
        return;
    }

    file_mgr_close(&mgr, first);
    file_mgr_destroy(&mgr);
    pass(name);
}

static void test_file_manager_closes_fp_when_no_free_slot(void) {
    const char *name = "file manager closes fp when no free slot";
    file_manager_t mgr;
    bool is_new = false;
    char path_a[256] = {0};
    char path_b[256] = {0};
    FILE *tracked = NULL;
    FILE *overflow = NULL;

    reset_test_dir();
    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    tracked = file_mgr_get_handle(&mgr, "slot", "one", 1, 0, path_a, sizeof(path_a), &is_new);
    if (tracked == NULL) {
        file_mgr_destroy(&mgr);
        fail(name, "failed to open tracked handle");
        return;
    }

    for (uint32_t i = 1; i < MAX_OPEN_FILES; ++i) {
        mgr.files[i].in_use = true;
        mgr.files[i].fp = NULL;
    }

    overflow = file_mgr_get_handle(&mgr, "slot", "two", 2, 0, path_b, sizeof(path_b), &is_new);
    if (overflow != NULL || mgr.count != 1) {
        file_mgr_destroy(&mgr);
        fail(name, "tail fclose branch did not reject cleanly");
        return;
    }

    file_mgr_destroy(&mgr);
    pass(name);
}

static void test_file_manager_rejects_when_full_and_no_idle_slot(void) {
    const char *name = "file manager rejects when full and no idle slot";
    file_manager_t mgr;
    bool is_new = false;
    char paths[MAX_OPEN_FILES + 1][256] = {{0}};
    FILE *handles[MAX_OPEN_FILES] = {0};
    FILE *overflow = NULL;

    reset_test_dir();
    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        char key2[16] = {0};
        snprintf(key2, sizeof(key2), "slot%u", i);
        handles[i] = file_mgr_get_handle(&mgr, "full", key2, i + 1, 0,
                                         paths[i], sizeof(paths[i]), &is_new);
        if (handles[i] == NULL) {
            for (uint32_t j = 0; j < i; ++j) {
                if (handles[j] != NULL) {
                    file_mgr_close(&mgr, handles[j]);
                }
            }
            file_mgr_destroy(&mgr);
            fail(name, "failed to fill open file table");
            return;
        }
    }

    overflow = file_mgr_get_handle(&mgr, "full", "overflow", MAX_OPEN_FILES + 1, 0,
                                   paths[MAX_OPEN_FILES], sizeof(paths[MAX_OPEN_FILES]), &is_new);
    if (overflow != NULL || mgr.count != MAX_OPEN_FILES) {
        for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
            if (handles[i] != NULL) {
                file_mgr_close(&mgr, handles[i]);
            }
        }
        file_mgr_destroy(&mgr);
        fail(name, "max open file rejection failed");
        return;
    }

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (handles[i] != NULL) {
            file_mgr_close(&mgr, handles[i]);
        }
    }
    file_mgr_destroy(&mgr);
    pass(name);
}

static void test_file_manager_close_rejects_foreign_handle(void) {
    const char *name = "file manager close rejects foreign handle";
    file_manager_t mgr;
    bool is_new = false;
    char tracked_path[256] = {0};
    char foreign_path[256] = {0};
    FILE *tracked = NULL;
    FILE *foreign = NULL;
    int rc = 0;

    reset_test_dir();
    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    tracked = file_mgr_get_handle(&mgr, "tracked", "one", 1, 0, tracked_path, sizeof(tracked_path), &is_new);
    snprintf(foreign_path, sizeof(foreign_path), "%sforeign.tmp", file_mgr_get_root_dir());
    foreign = fopen(foreign_path, "wb");
    if (tracked == NULL || foreign == NULL) {
        if (foreign != NULL) {
            fclose(foreign);
        }
        file_mgr_destroy(&mgr);
        fail(name, "failed to open tracked and foreign files");
        return;
    }

    rc = file_mgr_close(&mgr, foreign);
    if (rc != -1 || mgr.count != 1 || !file_mgr_is_tracking(&mgr, tracked)) {
        file_mgr_destroy(&mgr);
        fail(name, "foreign handle should be rejected");
        return;
    }

    fclose(foreign);
    file_mgr_close(&mgr, tracked);
    file_mgr_destroy(&mgr);
    pass(name);
}

static void test_file_manager_cleanup_filters_non_pcm_files(void) {
    const char *name = "file manager cleanup filters non pcm files";
    char old_pcm[512] = {0};
    char fresh_pcm[512] = {0};
    char note_txt[512] = {0};
    char dir_pcm[512] = {0};
    FILE *fp = NULL;

    reset_test_dir();
    snprintf(old_pcm, sizeof(old_pcm), "%sold_keepcheck.pcm", file_mgr_get_root_dir());
    snprintf(fresh_pcm, sizeof(fresh_pcm), "%sfresh_keepcheck.pcm", file_mgr_get_root_dir());
    snprintf(note_txt, sizeof(note_txt), "%snotes.txt", file_mgr_get_root_dir());
    snprintf(dir_pcm, sizeof(dir_pcm), "%snested_dir.pcm", file_mgr_get_root_dir());

    fp = fopen(old_pcm, "wb");
    if (fp != NULL) {
        fclose(fp);
    }
    fp = fopen(fresh_pcm, "wb");
    if (fp != NULL) {
        fclose(fp);
    }
    fp = fopen(note_txt, "wb");
    if (fp != NULL) {
        fclose(fp);
    }
    mkdir(dir_pcm, 0755);
    touch_old_file(old_pcm);
    touch_old_file(note_txt);

    if (file_mgr_cleanup(1, 1) != 0 || path_exists(old_pcm) || !path_exists(fresh_pcm) ||
        !path_exists(note_txt) || !path_exists(dir_pcm)) {
        remove(old_pcm);
        remove(fresh_pcm);
        remove(note_txt);
        rmdir(dir_pcm);
        fail(name, "cleanup filtered the wrong paths");
        return;
    }

    remove(fresh_pcm);
    remove(note_txt);
    rmdir(dir_pcm);
    pass(name);
}

static void test_get_active_sessions_argument_guards(void) {
    const char *name = "get active sessions argument guards";
    dump_session_info_t sessions[2] = {0};
    int actual = -1;

    if (dump_pcm_data_get_active_sessions(sessions, 2, NULL) != -1) {
        fail(name, "NULL actual_count should fail");
        return;
    }
    if (dump_pcm_data_get_active_sessions(NULL, 2, &actual) != -1) {
        fail(name, "NULL sessions should fail for positive max_count");
        return;
    }
    if (dump_pcm_data_get_active_sessions(NULL, 0, &actual) != 0 || actual != 0) {
        fail(name, "zero max_count should be a no-op");
        return;
    }

    pass(name);
}

static void test_logger_direct(void) {
    const char *name = "logger direct";

    g_logged_messages = 0;
    logger_init(NULL);
    logger_set_level(LOG_LEVEL_ERROR);
    logger_log(LOG_LEVEL_INFO, "filtered");
    logger_log(LOG_LEVEL_ERROR, "kept");
    logger_init(counting_log_fn);
    logger_set_level(LOG_LEVEL_INFO);
    logger_log(LOG_LEVEL_INFO, "counted");

    if (logger_get_level() != LOG_LEVEL_INFO || g_logged_messages != 1) {
        fail(name, "logger helper paths not covered correctly");
        return;
    }

    logger_init(pc_log_fn);
    logger_set_level(LOG_LEVEL_INFO);
    pass(name);
}

static void test_session_helper_direct(void) {
    const char *name = "session helper direct";
    session_manager_t mgr;
    session_info_t *first = NULL;
    session_info_t *second = NULL;

    session_mgr_init(&mgr);
    first = session_get_or_create(&mgr, "helper", "one");
    second = session_get_or_create(&mgr, "helper", "one");
    if (first == NULL || second != first || session_count(&mgr) != 1) {
        session_mgr_destroy(&mgr);
        fail(name, "session_get_or_create or session_count failed");
        return;
    }

    session_mgr_destroy(&mgr);
    pass(name);
}

static void touch_old_file(const char *path) {
    struct utimbuf times;
    times.actime = time(NULL) - 48 * 3600;
    times.modtime = times.actime;
    utime(path, &times);
}

static void test_file_manager_cleanup_and_evict(void) {
    const char *name = "file manager cleanup and evict";
    file_manager_t mgr;
    bool is_new = false;
    char conflict_path[256] = {0};
    char path_a[256] = {0};
    char path_b[256] = {0};
    char path_c[256] = {0};
    FILE *fp_a = NULL;
    FILE *fp_b = NULL;
    FILE *fp_c = NULL;
    FILE *conflict = NULL;

    reset_test_dir();
    snprintf(conflict_path, sizeof(conflict_path), "%sconflict", file_mgr_get_root_dir());
    conflict = fopen(conflict_path, "wb");
    if (conflict != NULL) {
        fclose(conflict);
    }

    file_mgr_set_root_dir(conflict_path);
    if (file_mgr_cleanup(1, 1) != -1) {
        remove(conflict_path);
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected cleanup failure for file path conflict");
        return;
    }
    if (file_mgr_init(&mgr) == 0) {
        remove(conflict_path);
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected init failure for file path conflict");
        return;
    }
    remove(conflict_path);
    file_mgr_set_root_dir("/tmp/audio_dump_test");

    if (file_mgr_init(&mgr) != 0) {
        fail(name, "file manager init failed");
        return;
    }

    fp_a = file_mgr_get_handle(&mgr, "evict", "a", 1, 0, path_a, sizeof(path_a), &is_new);
    fp_b = file_mgr_get_handle(&mgr, "evict", "b", 2, 0, path_b, sizeof(path_b), &is_new);
    if (fp_a == NULL || fp_b == NULL) {
        file_mgr_destroy(&mgr);
        fail(name, "failed to create initial files");
        return;
    }

    sleep(IDLE_TIMEOUT_SEC + 1);
    fp_c = file_mgr_get_handle(&mgr, "evict", "c", 3, 0, path_c, sizeof(path_c), &is_new);
    if (fp_c == NULL || mgr.count != MAX_OPEN_FILES) {
        file_mgr_destroy(&mgr);
        fail(name, "idle eviction did not free a slot");
        return;
    }

    file_mgr_close(&mgr, fp_b);
    file_mgr_close(&mgr, fp_c);

    fp_a = fopen(path_a, "ab");
    fp_b = fopen(path_b, "ab");
    if (fp_a != NULL) {
        fclose(fp_a);
    }
    if (fp_b != NULL) {
        fclose(fp_b);
    }
    touch_old_file(path_a);
    touch_old_file(path_b);
    if (file_mgr_cleanup(0, 0) != 0 || count_pcm_files() != 1) {
        file_mgr_destroy(&mgr);
        fail(name, "cleanup did not prune old files");
        return;
    }

    file_mgr_destroy(&mgr);
    remove(path_c);
    pass(name);
}

static void test_file_manager_init_and_open_failures(void) {
    const char *name = "file manager init and open failures";
    file_manager_t mgr;
    bool is_new = false;
    char path[256] = {0};
    FILE *fp = NULL;

    file_mgr_set_root_dir("/proc/forbidden/subdir");
    if (file_mgr_init(&mgr) == 0) {
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected intermediate mkdir failure");
        return;
    }

    file_mgr_set_root_dir("/proc/forbidden");
    if (file_mgr_init(&mgr) == 0) {
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected final mkdir failure");
        return;
    }

    file_mgr_set_root_dir("/proc");
    if (file_mgr_init(&mgr) != 0) {
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected /proc init to succeed");
        return;
    }

    fp = file_mgr_get_handle(&mgr, "openfail", "one", 1, 0, path, sizeof(path), &is_new);
    if (fp != NULL) {
        file_mgr_close(&mgr, fp);
        file_mgr_destroy(&mgr);
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected open failure under /proc");
        return;
    }

    file_mgr_destroy(&mgr);
    file_mgr_set_root_dir("/tmp/audio_dump_test");
    pass(name);
}

static void test_file_manager_init_creates_and_destroy_closes(void) {
    const char *name = "file manager init creates and destroy closes";
    file_manager_t mgr;
    bool is_new = false;
    char root_dir[256] = {0};
    char path_a[256] = {0};
    char path_b[256] = {0};
    FILE *fp_a = NULL;
    FILE *fp_b = NULL;

    snprintf(root_dir, sizeof(root_dir), "/tmp/audio_dump_newdir_%ld", (long)getpid());
    reset_test_dir();
    file_mgr_set_root_dir(root_dir);
    if (file_mgr_init(&mgr) != 0) {
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "expected init to create a new directory");
        return;
    }

    fp_a = file_mgr_get_handle(&mgr, "destroy", "a", 1, 0, path_a, sizeof(path_a), &is_new);
    fp_b = file_mgr_get_handle(&mgr, "destroy", "b", 2, 0, path_b, sizeof(path_b), &is_new);
    if (fp_a == NULL || fp_b == NULL) {
        file_mgr_destroy(&mgr);
        file_mgr_set_root_dir("/tmp/audio_dump_test");
        fail(name, "failed to open tracked handles");
        return;
    }

    file_mgr_destroy(&mgr);
    rmdir(root_dir);
    file_mgr_set_root_dir("/tmp/audio_dump_test");
    pass(name);
}

static int run_isolated_pthread_create_failure_retry(void) {
    dump_status_t status = {0};

    dump_pcm_set_pthread_create_fn_for_test(fake_pthread_create_fail);
    dump_pcm_data_write("t", 1, "pthread", "fail");
    if (dump_pcm_data_get_status(&status) != -1) {
        return 1;
    }

    dump_pcm_data_write("t", 1, "pthread", "too_soon");
    usleep(100000);
    if (dump_pcm_data_get_status(&status) != -1) {
        return 1;
    }

    usleep(600000);
    dump_pcm_set_pthread_create_fn_for_test(NULL);
    dump_pcm_data_write("t", 1, "pthread", "ok");
    usleep(80000);
    if (dump_pcm_data_get_status(&status) != 0) {
        return 1;
    }

    dump_pcm_data_stop("pthread", "ok");
    return 0;
}

static int run_isolated_init_failure_test(void) {
    dump_status_t status = {0};

    file_mgr_set_root_dir("/dev/null/subdir");
    dump_pcm_data_write("x", 1, "bad", "init");
    if (dump_pcm_data_get_status(&status) != -1) {
        return 1;
    }

    return 0;
}

static int run_isolated_setclock_fallback_test(void) {
    dump_status_t status = {0};
    char paths[4][512];
    char content[8] = {0};
    int before = 0;
    int after = 0;
    int count = 0;

    reset_test_dir();
    set_fail_kind(FAIL_KIND_PTHREAD_CONDATTR_SETCLOCK, 0);
    dump_pcm_data_write("x", 1, "fallback", "clock");
    clear_fail_kind();

    if (dump_pcm_data_get_status(&status) != 0 || !status.enabled) {
        return 1;
    }

    stop_and_wait("fallback", "clock");

    count = find_pcm_files(paths, 4);
    if (count != 1) {
        return 1;
    }
    if (read_file(paths[0], content, sizeof(content)) != 1 || content[0] != 'x') {
        return 1;
    }

    /* A mismatched cond clock turns the idle wait into a busy loop; bound the
     * number of timed waits observed over a 200ms idle window. */
    before = atomic_load(&g_cond_timedwait_calls);
    usleep(200000);
    after = atomic_load(&g_cond_timedwait_calls);
    if (after - before > 50) {
        return 1;
    }

    return 0;
}

static int run_isolated_cleanup_deferred_test(void) {
    dump_status_t status = {0};

    reset_test_dir();
    g_caller_thread = pthread_self();
    atomic_store(&g_dir_ops_on_caller, 0);
    atomic_store(&g_count_dir_ops, 1);
    dump_pcm_data_write("x", 1, "deferred", "cleanup");
    atomic_store(&g_count_dir_ops, 0);

    /* The writer thread also scans the directory, so only count opendir calls
     * made from the thread that entered dump_pcm_data_write(). */
    if (atomic_load(&g_dir_ops_on_caller) != 0) {
        return 1;
    }

    stop_and_wait("deferred", "cleanup");

    if (count_pcm_files() != 1) {
        return 1;
    }
    if (dump_pcm_data_get_status(&status) != 0) {
        return 1;
    }

    return 0;
}

static int run_isolated_disabled_write_test(void) {
    dump_status_t status = {0};
    char dir[256] = {0};

    snprintf(dir, sizeof(dir), "/tmp/audio_dump_test/disabled_%d/", (int)getpid());
    rmdir(dir);
    file_mgr_set_root_dir(dir);

    dump_pcm_data_write("x", 1, "disabled", "case");

    /* Initialization creates the root directory, so its absence proves the
     * module never initialized while the switch was off. */
    if (path_exists(dir)) {
        return 1;
    }
    if (dump_pcm_data_get_status(&status) != -1) {
        return 1;
    }

    return 0;
}

static int run_isolated_init_failpoint_test(void) {
    const char *failpoint = getenv("AUDIO_DUMP_INIT_FAILPOINT");
    dump_status_t status = {0};

    if (failpoint == NULL) {
        return 2;
    }

    if (strcmp(failpoint, "mutex") == 0) {
        set_fail_kind(FAIL_KIND_PTHREAD_MUTEX_INIT, 0);
    } else if (strcmp(failpoint, "condattr") == 0) {
        set_fail_kind(FAIL_KIND_PTHREAD_CONDATTR_INIT, 0);
    } else if (strcmp(failpoint, "clock") == 0) {
        set_fail_kind(FAIL_KIND_PTHREAD_CONDATTR_SETCLOCK, 0);
    } else if (strcmp(failpoint, "cond") == 0) {
        set_fail_kind(FAIL_KIND_PTHREAD_COND_INIT, 0);
    } else {
        return 2;
    }

    dump_pcm_data_write("x", 1, "bad", "init");
    if (dump_pcm_data_get_status(&status) != -1) {
        clear_fail_kind();
        return 1;
    }

    clear_fail_kind();
    return 0;
}

static int run_isolated_session_capacity_test(void) {
    dump_status_t status = {0};
    char key1[32] = {0};
    char key2[32] = {0};

    reset_test_dir();
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        snprintf(key1, sizeof(key1), "cap_pub%u", i);
        snprintf(key2, sizeof(key2), "slot_pub%u", i);
        dump_pcm_data_write("x", 1, key1, key2);
        usleep(2000);
    }

    dump_pcm_data_write("x", 1, "overflow", "reject");
    usleep(80000);
    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != MAX_SESSIONS) {
        return 1;
    }

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        snprintf(key1, sizeof(key1), "cap_pub%u", i);
        snprintf(key2, sizeof(key2), "slot_pub%u", i);
        stop_and_wait(key1, key2);
    }

    return 0;
}

static void test_init_retry_path(void) {
    const char *name = "init retry path";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_PTHREAD_CREATE_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated retry path failed");
        return;
    }

    pass(name);
}

static void test_init_failure_path(void) {
    const char *name = "init failure path";
    pid_t pid = fork();
    int status = 0;
    char conflict_path[256] = {0};

    snprintf(conflict_path, sizeof(conflict_path), "/dev/null/subdir");

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_DIR", conflict_path, 1);
        setenv("AUDIO_DUMP_RUN_INIT_FAILURE_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated init failure path failed");
        return;
    }

    pass(name);
}

static void test_init_do_init_failures(void) {
    const char *name = "init do_init failures";
    /* "clock" is no longer fatal at init: the module falls back to CLOCK_REALTIME. */
    const char *modes[] = {"mutex", "condattr", "cond"};

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        pid_t pid = fork();
        int status = 0;

        if (pid == 0) {
            setenv("AUDIO_DUMP_ENABLED", "1", 1);
            setenv("AUDIO_DUMP_RUN_INIT_FAILPOINT_TEST", "1", 1);
            setenv("AUDIO_DUMP_INIT_FAILPOINT", modes[i], 1);
            execl("./dump_pcm_test", "./dump_pcm_test", NULL);
            _exit(2);
        }

        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fail(name, modes[i]);
            return;
        }
    }

    pass(name);
}

static void test_init_setclock_fallback(void) {
    const char *name = "init setclock fallback";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_SETCLOCK_FALLBACK_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated setclock fallback path failed");
        return;
    }

    pass(name);
}

static void test_disabled_write_has_no_side_effects(void) {
    const char *name = "disabled write has no side effects";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "0", 1);
        setenv("AUDIO_DUMP_RUN_DISABLED_WRITE_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "disabled write produced init side effects");
        return;
    }

    pass(name);
}

static void test_cleanup_deferred_to_writer_thread(void) {
    const char *name = "cleanup deferred to writer thread";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_CLEANUP_DEFERRED_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "caller thread performed a directory scan");
        return;
    }

    pass(name);
}

static void test_init_wait_path(void) {
    const char *name = "init wait path";
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("AUDIO_DUMP_ENABLED", "1", 1);
        setenv("AUDIO_DUMP_RUN_INIT_WAIT_TEST", "1", 1);
        execl("./dump_pcm_test", "./dump_pcm_test", NULL);
        _exit(2);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "isolated init wait path failed");
        return;
    }

    pass(name);
}

static int run_isolated_init_wait_test(void) {
    dump_status_t status = {0};

    reset_test_dir();
    g_init_wait_probe_started = false;
    g_init_wait_probe_ready = 0;
    dump_pcm_set_pthread_create_fn_for_test(fake_pthread_create_with_probe);
    dump_pcm_data_write("main", 4, "init_wait", "same");
    usleep(400000);
    dump_pcm_set_pthread_create_fn_for_test(NULL);
    if (g_init_wait_probe_started) {
        pthread_join(g_init_wait_probe_thread, NULL);
    }
    stop_and_wait("init_wait", "same");

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 0) {
        return 1;
    }

    return 0;
}

static void test_whitebox_coverage_shim(void) {
    const char *name = "whitebox coverage shim";

    if (run_whitebox_coverage_shim() != 0) {
        fail(name, "whitebox shim failed");
        return;
    }

    pass(name);
}

int main(void) {
    const char *base_dir = NULL;

    if (getenv("AUDIO_DUMP_RUN_PTHREAD_CREATE_TEST") != NULL) {
        return run_isolated_pthread_create_failure_retry();
    }

    if (getenv("AUDIO_DUMP_RUN_INIT_FAILURE_TEST") != NULL) {
        return run_isolated_init_failure_test();
    }

    if (getenv("AUDIO_DUMP_RUN_INIT_FAILPOINT_TEST") != NULL) {
        return run_isolated_init_failpoint_test();
    }

    if (getenv("AUDIO_DUMP_RUN_SETCLOCK_FALLBACK_TEST") != NULL) {
        return run_isolated_setclock_fallback_test();
    }

    if (getenv("AUDIO_DUMP_RUN_DISABLED_WRITE_TEST") != NULL) {
        return run_isolated_disabled_write_test();
    }

    if (getenv("AUDIO_DUMP_RUN_CLEANUP_DEFERRED_TEST") != NULL) {
        return run_isolated_cleanup_deferred_test();
    }

    if (getenv("AUDIO_DUMP_RUN_PUBLIC_WRITE_ALLOC_TEST") != NULL) {
        return run_isolated_public_write_allocation_failures();
    }

    if (getenv("AUDIO_DUMP_RUN_QUEUE_POP_TEST") != NULL) {
        return run_isolated_writer_queue_pop_failure();
    }

    if (getenv("AUDIO_DUMP_RUN_QUEUE_FULL_TEST") != NULL) {
        return run_isolated_queue_full_write_rejection();
    }

    if (getenv("AUDIO_DUMP_RUN_QUEUE_FULL_CLOCK_TEST") != NULL) {
        return run_isolated_queue_full_failed_enqueue_clock_test();
    }

    if (getenv("AUDIO_DUMP_RUN_INIT_WAIT_TEST") != NULL) {
        return run_isolated_init_wait_test();
    }

    if (getenv("AUDIO_DUMP_RUN_SESSION_CAP_TEST") != NULL) {
        return run_isolated_session_capacity_test();
    }

    if (getenv("AUDIO_DUMP_RUN_STOP_PENDING_TEST") != NULL) {
        return run_isolated_stop_while_pending_blocks();
    }

    logger_init(pc_log_fn);
    logger_set_level(LOG_LEVEL_INFO);
    base_dir = getenv("AUDIO_DUMP_DIR") != NULL ? getenv("AUDIO_DUMP_DIR") : "/tmp/audio_dump_test";
    strncpy(g_test_dir_base, base_dir, sizeof(g_test_dir_base) - 1);
    g_test_dir_base[sizeof(g_test_dir_base) - 1] = '\0';
    file_mgr_set_root_dir(g_test_dir_base);
    mkdir(file_mgr_get_root_dir(), 0755);
    setenv("AUDIO_DUMP_ENABLED", "1", 1);

    printf("========================================\n");
    printf("   PCM Dump Module Test Suite\n");
    printf("========================================\n");

    test_string_key_write_and_escape();
    test_key_validation();
    test_argument_guard_coverage();
    test_storage_cache_interval_hits_cached_path();
    test_storage_cache_hits_cached_under_lock();
    test_session_separation_and_query();
    test_stop_then_new_session_creates_new_file();
    test_write_gap_exactly_two_seconds_keeps_same_file();
    test_write_gap_over_two_seconds_creates_new_file();
    test_low_storage_latch_requires_reenable();
    test_filename_hash_disambiguates_escaped_collisions();
    test_same_timestamp_rollovers_get_unique_files();
    test_statvfs_failure_blocks_new_session();
    test_statvfs_low_storage_blocks_new_session();
    test_statvfs_large_storage_reports_full_value();
    test_queue_direct();
    test_queue_empty_pop_and_byte_limit();
    test_session_direct();
    test_session_highest_generation_lookup();
    test_queue_allocation_failures();
    test_session_allocation_failures();
    test_public_write_allocation_failures();
    test_writer_queue_pop_failure();
    test_writer_queue_full_write_rejection();
    test_writer_queue_full_failed_enqueue_keeps_rollover_clock();
    test_session_capacity_rejection();
    test_public_write_rejects_when_session_capacity_reached();
    test_stop_while_pending_blocks();
    test_session_helper_direct();
    test_file_manager_direct();
    test_file_manager_reuses_tracked_handle();
    test_file_manager_closes_fp_when_no_free_slot();
    test_file_manager_idle_scan_skips_inactive_slots();
    test_file_manager_rejects_when_full_and_no_idle_slot();
    test_file_manager_close_rejects_foreign_handle();
    test_file_manager_cleanup_filters_non_pcm_files();
    test_get_active_sessions_argument_guards();
    test_file_manager_cleanup_and_evict();
    test_file_manager_init_and_open_failures();
    test_file_manager_init_creates_and_destroy_closes();
    test_file_manager_cleanup_realloc_failure();
    test_logger_direct();
    test_init_retry_path();
    test_init_failure_path();
    test_init_do_init_failures();
    test_init_setclock_fallback();
    test_disabled_write_has_no_side_effects();
    test_cleanup_deferred_to_writer_thread();
    test_init_wait_path();
    test_whitebox_coverage_shim();

    printf("\n========================================\n");
    printf("   Test Results\n");
    printf("========================================\n");
    printf("Tests Passed: %d\n", tests_passed);
    printf("Tests Failed: %d\n", tests_failed);
    printf("Total Tests:  %d\n", tests_passed + tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
