#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define DUMP_PCM_INTERNAL_DEBUG_API 1

#include "../include/dump_pcm_data.h"
#include "../src/dump_internal.h"
#include "../src/dump_test_hooks.h"
#include "../src/file_mgr.h"
#include "../src/key_utils.h"
#include "../src/logger.h"
#include "../src/queue.h"
#include "../src/session.h"
#include "../src/sidecar.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include "proc_iso.h"
#include "test_plat.h"

int run_whitebox_coverage_shim(void);

static int tests_passed = 0;
static int tests_failed = 0;
static char g_test_dir_base[256] = {0};
static int g_test_dir_seq = 0;
/* Set when a fixture could not establish the state a case is about to assert
 * on. A case running against a fixture that silently failed would otherwise
 * report PASS for having observed nothing.
 *
 * Measured with a root whose parent is a regular file, so no case can create
 * its directory: 51 of 72 fail and 21 still pass. Sixteen of those never touch
 * the filesystem; one asserts that a write to an unusable path fails, which a
 * broken root only makes more true. The remaining five assert the *absence*
 * of a side effect - `key validation`, `storage failure blocks new session`,
 * `init failure path`, `init do_init failures`, `disabled write has no side
 * effects` - and cannot tell "nothing happened" from "nothing could happen".
 * They stay honest only because a broken root turns most of the suite red
 * first. Do not read their PASS as evidence that the mechanism under test ran.
 * CI pins the 51 with `fails -lt 51`; re-measure both numbers when cases are
 * added. */
static const char *g_fixture_error = NULL;

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
    /* A broken fixture outranks the case's own verdict: whatever it observed,
     * it observed against the wrong state. */
    if (g_fixture_error != NULL) {
        printf("FAIL: %s, %s\n", name, g_fixture_error);
        tests_failed++;
        g_fixture_error = NULL;
        return;
    }

    printf("PASS: %s\n", name);
    tests_passed++;
}

static void fail(const char *name, const char *why) {
    printf("FAIL: %s, %s\n", name, why);
    tests_failed++;
    /* Drop any pending fixture error: this case already reported. */
    g_fixture_error = NULL;
}

static void run_isolated_case(const char *name, const char *const *env,
                              const char *why) {
    iso_result_t result;

    if (iso_run(env, &result) != 0 || !iso_ok(&result)) {
        fail(name, why);
        return;
    }
    pass(name);
}

static int fake_storage_fail(const char *path, uint64_t *available_bytes) {
    (void)path;
    (void)available_bytes;
    errno = EIO;
    return -1;
}

static int fake_storage_low(const char *path, uint64_t *available_bytes) {
    (void)path;
    *available_bytes = 1ULL * 1024ULL * 1024ULL;
    return 0;
}

static int fake_storage_large(const char *path, uint64_t *available_bytes) {
    (void)path;
    *available_bytes = 33792ULL * 1024ULL * 1024ULL;
    return 0;
}

/* Counts how many times the module actually asked for free space. The storage
 * cache is only observable through this number: a cached answer and a fresh
 * query return the same value, so a case that asserts state alone cannot tell
 * them apart. */
static atomic_int g_storage_query_calls = 0;

static int fake_storage_counting(const char *path, uint64_t *available_bytes) {
    (void)path;
    atomic_fetch_add(&g_storage_query_calls, 1);
    *available_bytes = 1024ULL * 1024ULL * 1024ULL;
    return 0;
}

static int fake_storage_slow(const char *path, uint64_t *available_bytes) {
    (void)path;
    atomic_fetch_add(&g_storage_query_calls, 1);
    usleep(50000);
    *available_bytes = 1024ULL * 1024ULL * 1024ULL;
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

/* ---- Stage-2 deterministic lock-order audit (spec section 13.1) ---------
 *
 * The evidence probe used two barriers and an ABBA table. This in-suite form
 * asserts the same thing directly: a per-thread "F is held" flag plus a count
 * of every C acquisition that happens while F is held. Such an acquisition IS
 * the F->C edge that would turn the C->F DAG into a cycle, so the judgement is
 * on lock ownership and cannot depend on OS timing.
 *
 * Both wrappers are a plain pass-through while g_lock_audit_active is off, so
 * the other cases see unchanged pthread behaviour. */
static atomic_int g_lock_audit_active = 0;
static atomic_int g_f_to_c_edges = 0;
static atomic_int g_f_takes = 0;
static atomic_int g_c_takes = 0;
static _Thread_local int t_f_held = 0;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);

static int audit_lock_kind(pthread_mutex_t *mutex) {
    if (mutex == (pthread_mutex_t *)file_mgr_mutex_addr_for_test()) {
        return 2; /* F: g_file_mgr_mutex */
    }
    if (mutex == &g_ctx.mutex) {
        return 1; /* C */
    }
    return 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    int kind = 0;
    int rc;

    if (atomic_load(&g_lock_audit_active) != 0) {
        kind = audit_lock_kind(mutex);
        if (kind == 2) {
            atomic_fetch_add(&g_f_takes, 1);
        } else if (kind == 1) {
            atomic_fetch_add(&g_c_takes, 1);
            if (t_f_held != 0) {
                atomic_fetch_add(&g_f_to_c_edges, 1);
            }
        }
    }
    rc = __real_pthread_mutex_lock(mutex);
    if (rc == 0 && kind == 2) {
        t_f_held = 1;
    }
    return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (atomic_load(&g_lock_audit_active) != 0 &&
        audit_lock_kind(mutex) == 2) {
        t_f_held = 0;
    }
    return __real_pthread_mutex_unlock(mutex);
}

static void lock_audit_reset(void) {
    atomic_store(&g_f_to_c_edges, 0);
    atomic_store(&g_f_takes, 0);
    atomic_store(&g_c_takes, 0);
    t_f_held = 0;
    atomic_store(&g_lock_audit_active, 1);
}

static void reset_test_dir(void) {
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    char path[512] = {0};

    if (g_test_dir_base[0] == '\0') {
        strncpy(g_test_dir_base, tp_test_root(), sizeof(g_test_dir_base) - 1);
        g_test_dir_base[sizeof(g_test_dir_base) - 1] = '\0';
    }

    snprintf(path, sizeof(path), "%s_case_%02d/", g_test_dir_base, g_test_dir_seq++);
    file_mgr_set_root_dir(path);

    /* Every case that fails to prepare its directory must not pass on the
     * absence of files: an unreadable directory and an empty one look the
     * same to count_pcm_files(). */
    if (tp_mkdir(file_mgr_get_root_dir()) != 0 && errno != EEXIST) {
        g_fixture_error = "cannot create the case directory";
        return;
    }

    dir = opendir(file_mgr_get_root_dir());
    if (dir == NULL) {
        g_fixture_error = "cannot open the case directory";
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
        /* -1, not 0: an unreadable directory must never be able to impersonate
         * "no files here". Every caller compares against an exact count, so
         * this reports as a failure rather than a quiet pass. */
        return -1;
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
        /* -1, not 0: an unreadable directory must never be able to impersonate
         * "no files here". Every caller compares against an exact count, so
         * this reports as a failure rather than a quiet pass. */
        return -1;
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

/* Pointer identity is unreliable here: a closed FILE's allocation is reused by
 * the next fopen, so eviction assertions compare slot paths instead. */
static bool any_slot_tracks_path(const file_manager_t *mgr, const char *path) {
    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (mgr->files[i].in_use && strcmp(mgr->files[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool path_exists(const char *path) {
    struct stat st = {0};
    return path != NULL && stat(path, &st) == 0;
}

static int count_json_files(void) {
    DIR *dir = opendir(file_mgr_get_root_dir());
    struct dirent *entry = NULL;
    int count = 0;

    if (dir == NULL) {
        /* -1, not 0: an unreadable directory must never be able to impersonate
         * "no files here". Every caller compares against an exact count, so
         * this reports as a failure rather than a quiet pass. */
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".json") != NULL) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

static void json_path_for_pcm(const char *pcm_path, char *out, size_t out_size) {
    size_t len = 0;

    if (pcm_path == NULL || out == NULL || out_size == 0) {
        return;
    }

    len = strlen(pcm_path);
    if (len + sizeof(".json") > out_size) {
        return;
    }
    memcpy(out, pcm_path, len + 1);
    if (len > 4 && strcmp(pcm_path + len - 4, ".pcm") == 0) {
        memcpy(out + len - 4, ".json", sizeof(".json"));
    }
}

static bool wait_for_sidecar(const char *json_path, int max_ms) {
    for (int waited = 0; waited <= max_ms; waited += 5) {
        if (path_exists(json_path)) {
            return true;
        }
        usleep(5000);
    }

    return path_exists(json_path);
}

/* Reads a numeric JSON field written by sidecar_build_json(), e.g.
 * "total_bytes": 4096. Returns false when the field is absent. */
static bool json_uint_field(const char *json, const char *field, uint64_t *out) {
    char needle[64];
    const char *p = NULL;
    char *end = NULL;

    snprintf(needle, sizeof(needle), "\"%s\":", field);
    p = strstr(json, needle);
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    while (*p == ' ') {
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }

    *out = strtoull(p, &end, 10);
    return end != p;
}

static void remove_case_artifacts(void) {
    DIR *dir = opendir(file_mgr_get_root_dir());
    struct dirent *entry = NULL;

    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[512];

        if (strstr(entry->d_name, ".pcm") == NULL && strstr(entry->d_name, ".json") == NULL) {
            continue;
        }
        snprintf(path, sizeof(path), "%s%s", file_mgr_get_root_dir(), entry->d_name);
        remove(path);
    }

    closedir(dir);
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
    char paths[4][512] = {{0}};
    char content[32] = {0};
    int files = 0;
    int calls = 0;

    reset_test_dir();
    /* A cold cache makes the first write query and the second one reuse the
     * answer. Without forcing that, an inherited warm cache from a previous
     * case would make the query count zero and the assertion meaningless. */
    atomic_store(&g_ctx.last_storage_check, 0);
    atomic_store(&g_storage_query_calls, 0);
    dump_pcm_set_storage_query_fn_for_test(fake_storage_counting);

    dump_pcm_data_write("cache", 5, "interval", "path");
    dump_pcm_data_write("cache2", 6, "interval", "path");
    usleep(120000);
    stop_and_wait("interval", "path");
    dump_pcm_set_storage_query_fn_for_test(NULL);

    files = find_pcm_files(paths, 4);
    if (files != 1 || read_file(paths[0], content, sizeof(content) - 1) != 11 ||
        memcmp(content, "cachecache2", 11) != 0) {
        fail(name, "both writes must land in one file despite the cache hit");
        return;
    }

    calls = atomic_load(&g_storage_query_calls);
    if (calls != 1) {
        fail(name, "the second write inside the interval must not re-query storage");
        return;
    }

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
    char paths[4][512] = {{0}};
    char content[8] = {0};
    int calls = 0;

    reset_test_dir();
    dump_pcm_data_write("warm", 4, "cache_lock", "same");
    usleep(120000);
    stop_and_wait("cache_lock", "same");
    /* Drop the warm-up file so the assertion below can compare against an exact
     * shard count; the storage cache lives in the module, not in the directory. */
    reset_test_dir();
    /* Cold cache without a 1.1 s sleep: the query below must be the only one,
     * which is what proves the second thread was served from the cache while
     * the first held the storage mutex. */
    atomic_store(&g_ctx.last_storage_check, 0);
    atomic_store(&g_storage_query_calls, 0);
    dump_pcm_set_storage_query_fn_for_test(fake_storage_slow);
    if (pthread_barrier_init(&barrier, NULL, 3) != 0) {
        dump_pcm_set_storage_query_fn_for_test(NULL);
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
        dump_pcm_set_storage_query_fn_for_test(NULL);
        fail(name, "failed to start concurrent writers");
        return;
    }

    pthread_barrier_wait(&barrier);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    pthread_barrier_destroy(&barrier);
    dump_pcm_set_storage_query_fn_for_test(NULL);
    usleep(120000);
    stop_and_wait("cache_lock", "same");

    calls = atomic_load(&g_storage_query_calls);
    if (calls != 1) {
        fail(name, "two racing writers must share one storage query");
        return;
    }

    if (find_pcm_files(paths, 4) != 1 || read_file(paths[0], content, sizeof(content) - 1) != 2) {
        fail(name, "both concurrent payloads must land in one file");
        return;
    }

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
    tp_setenv("AUDIO_DUMP_TEST_AVAILABLE_MB", "100");
    dump_pcm_data_write("low", 3, "low_key", "one");
    usleep(60000);
    dump_pcm_data_get_status(&status);
    if (!status.low_storage_triggered) {
        fail(name, "low storage latch did not trigger");
        return;
    }

    tp_setenv("AUDIO_DUMP_TEST_AVAILABLE_MB", "1000");
    {
        uint64_t dropped_before = status.dropped_blocks;

        dump_pcm_data_write("low", 3, "low_key", "two");
        usleep(60000);
        dump_pcm_data_get_status(&status);
        if (!status.low_storage_triggered || status.active_session_count != 0) {
            fail(name, "latch cleared without re-enable");
            return;
        }
        /* A block refused while the feature is on is a loss, and the caller has
         * to be able to see it: this counter is what any later decision about
         * dropping more aggressively would be measured against. */
        if (status.dropped_blocks != dropped_before + 1) {
            fail(name, "dropped block was not counted");
            return;
        }
    }

    tp_unsetenv("AUDIO_DUMP_ENABLED");
    dump_pcm_data_write("low", 3, "low_key", "three");
    usleep(30000);
    tp_setenv("AUDIO_DUMP_ENABLED", "1");
    dump_pcm_data_write("low", 3, "low_key", "three");
    usleep(80000);
    dump_pcm_data_get_status(&status);
    tp_unsetenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
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

static void test_storage_failure_blocks_new_session(void) {
    const char *name = "storage query failure blocks new session";
    dump_status_t before = {0};
    dump_status_t after = {0};

    reset_test_dir();
    tp_unsetenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
    usleep(1100000);
    dump_pcm_data_get_status(&before);
    dump_pcm_set_storage_query_fn_for_test(fake_storage_fail);
    dump_pcm_data_write("s", 1, "storage", "fail");
    usleep(60000);
    dump_pcm_set_storage_query_fn_for_test(NULL);
    dump_pcm_data_get_status(&after);

    if (after.active_session_count != before.active_session_count) {
        fail(name, "storage query failure admitted a session");
        return;
    }

    pass(name);
}

static void test_storage_low_storage_blocks_new_session(void) {
    const char *name = "storage query low storage blocks new session";
    dump_status_t status = {0};

    reset_test_dir();
    dump_pcm_set_storage_query_fn_for_test(fake_storage_low);
    dump_pcm_data_write("s", 1, "storage", "low");
    usleep(60000);
    dump_pcm_set_storage_query_fn_for_test(NULL);
    if (dump_pcm_data_get_status(&status) != 0 || status.low_storage_triggered != 1 ||
        status.active_session_count != 0) {
        fail(name, "low storage branch did not latch correctly");
        return;
    }

    pass(name);
}

static void test_storage_large_storage_reports_full_value(void) {
    const char *name = "storage query large storage reports full value";
    dump_status_t status = {0};

    reset_test_dir();
    tp_unsetenv("AUDIO_DUMP_ENABLED");
    dump_pcm_data_write("s", 1, "storage", "reset");
    usleep(30000);
    tp_setenv("AUDIO_DUMP_ENABLED", "1");
    tp_unsetenv("AUDIO_DUMP_TEST_AVAILABLE_MB");
    usleep(1100000);
    dump_pcm_set_storage_query_fn_for_test(fake_storage_large);
    dump_pcm_data_write("s", 1, "storage", "large");
    usleep(60000);
    dump_pcm_set_storage_query_fn_for_test(NULL);

    if (dump_pcm_data_get_status(&status) != 0 || status.available_storage_mb != 33792 ||
        status.active_session_count != 1) {
        stop_and_wait("storage", "large");
        fail(name, "expected full 64-bit storage value to be preserved");
        return;
    }

    stop_and_wait("storage", "large");
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

static void test_session_file_action_policy(void) {
    const char *name = "session file action policy";
    session_info_t session = {0};

    if (session_file_action(NULL, false, 1024) != SESSION_FILE_REOPEN ||
        session_file_action(&session, false, 1024) != SESSION_FILE_REOPEN) {
        fail(name, "missing handle must reopen");
        return;
    }

    session.fp = stdout;
    if (session_file_action(&session, false, 1024) != SESSION_FILE_REOPEN) {
        fail(name, "untracked handle must reopen");
        return;
    }
    if (session_file_action(&session, true, 1024) != SESSION_FILE_USE) {
        fail(name, "tracked handle below the limit must be reused");
        return;
    }

    session.file_size = 1024;
    if (session_file_action(&session, true, 1024) != SESSION_FILE_ROLLOVER) {
        fail(name, "size limit must roll over");
        return;
    }
    if (session_file_action(&session, false, 1024) != SESSION_FILE_REOPEN) {
        fail(name, "evicted handle wins over the size limit");
        return;
    }

    pass(name);
}

static void test_format_validation(void) {
    const char *name = "pcm format validation";
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};

    if (!session_format_is_valid(&fmt) || session_format_is_valid(NULL)) {
        fail(name, "baseline validity wrong");
        return;
    }

    fmt.bits_per_sample = 24;
    if (session_format_is_valid(&fmt)) {
        fail(name, "mismatched bit depth accepted");
        return;
    }

    fmt.bits_per_sample = 16;
    fmt.channel_count = 0;
    if (session_format_is_valid(&fmt)) {
        fail(name, "zero channels accepted");
        return;
    }

    fmt.channel_count = 2;
    fmt.sample_rate = 7999;
    if (session_format_is_valid(&fmt)) {
        fail(name, "out-of-range rate accepted");
        return;
    }

    fmt.sample_rate = 48000;
    fmt.sample_format = DUMP_PCM_FORMAT_UNKNOWN;
    if (session_format_is_valid(&fmt)) {
        fail(name, "unknown format accepted");
        return;
    }

    fmt.sample_format = DUMP_PCM_FORMAT_FLOAT_LE;
    fmt.bits_per_sample = 32;
    if (!session_format_is_valid(&fmt)) {
        fail(name, "valid float format rejected");
        return;
    }

    pass(name);
}

static void test_write_ex_captures_first_format(void) {
    const char *name = "write_ex captures the first format";
    dump_pcm_format_t first = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};
    dump_pcm_format_t second = {44100, 1, 24, DUMP_PCM_FORMAT_S24_LE};
    dump_session_info_t sessions[4] = {{0}};
    int count = 0;
    int idx = -1;

    reset_test_dir();
    dump_pcm_data_write_ex("ab", 2, "fmt", "cap", &first);
    dump_pcm_data_write_ex("cd", 2, "fmt", "cap", &second);
    usleep(120000);

    if (dump_pcm_data_get_active_sessions(sessions, 4, &count) != 0) {
        fail(name, "session query failed");
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (strcmp(sessions[i].key1, "fmt") == 0 && strcmp(sessions[i].key2, "cap") == 0) {
            idx = i;
        }
    }
    if (idx < 0) {
        fail(name, "session missing");
        return;
    }
    if (!sessions[idx].has_format || sessions[idx].format.sample_rate != 48000 ||
        sessions[idx].format.channel_count != 2 ||
        sessions[idx].format.sample_format != DUMP_PCM_FORMAT_S16_LE) {
        stop_and_wait("fmt", "cap");
        fail(name, "first format was not kept");
        return;
    }

    stop_and_wait("fmt", "cap");
    pass(name);
}

static void test_write_ex_without_format_matches_legacy(void) {
    const char *name = "write_ex without format matches legacy";
    char files[4][512] = {{0}};
    char legacy[8] = {0};
    char via_ex[8] = {0};
    int count = 0;

    reset_test_dir();
    dump_pcm_data_write("abc", 3, "eq", "legacy");
    stop_and_wait("eq", "legacy");
    count = find_pcm_files(files, 4);
    if (count != 1 || read_file(files[0], legacy, sizeof(legacy)) != 3) {
        fail(name, "legacy write produced unexpected output");
        return;
    }

    reset_test_dir();
    dump_pcm_data_write_ex("abc", 3, "eq", "ex", NULL);
    stop_and_wait("eq", "ex");
    count = find_pcm_files(files, 4);
    if (count != 1 || read_file(files[0], via_ex, sizeof(via_ex)) != 3) {
        fail(name, "write_ex produced unexpected output");
        return;
    }

    if (memcmp(legacy, via_ex, 3) != 0) {
        fail(name, "byte content differs");
        return;
    }

    pass(name);
}

static void test_sidecar_utf8_validation(void) {
    const char *name = "sidecar utf8 validation";

    if (!sidecar_is_valid_utf8("abc") || !sidecar_is_valid_utf8("中文") ||
        sidecar_is_valid_utf8(NULL)) {
        fail(name, "baseline validation wrong");
        return;
    }
    if (sidecar_is_valid_utf8("\xC0\xAF")) {
        fail(name, "overlong sequence accepted");
        return;
    }
    if (sidecar_is_valid_utf8("\x80")) {
        fail(name, "lone continuation accepted");
        return;
    }
    if (sidecar_is_valid_utf8("\xE4\xB8")) {
        fail(name, "truncated sequence accepted");
        return;
    }
    if (sidecar_is_valid_utf8("\xED\xA0\x80")) {
        fail(name, "surrogate accepted");
        return;
    }

    pass(name);
}

static void test_sidecar_json_builder(void) {
    const char *name = "sidecar json builder";
    dump_pcm_file_meta_t meta = {0};
    char buf[512] = {0};
    int len = 0;

    snprintf(meta.key1, sizeof(meta.key1), "out\"a");
    snprintf(meta.key2, sizeof(meta.key2), "b\\c");
    meta.generation = 3;
    meta.split_index = 1;
    meta.format.sample_rate = 48000;
    meta.format.channel_count = 2;
    meta.format.bits_per_sample = 16;
    meta.format.sample_format = DUMP_PCM_FORMAT_S16_LE;
    meta.start_time_unix_ms = 1776397812123ULL;

    len = sidecar_build_json(buf, sizeof(buf), "x.pcm", &meta, true, 1048576ULL, 1776397825871ULL);
    if (len <= 0) {
        fail(name, "complete json failed");
        return;
    }
    if (strstr(buf, "\"total_bytes\": 1048576") == NULL ||
        strstr(buf, "\"complete\": true") == NULL ||
        strstr(buf, "\"sample_format\": \"s16_le\"") == NULL ||
        strstr(buf, "\"key1\": \"out\\\"a\"") == NULL ||
        strstr(buf, "\"key2\": \"b\\\\c\"") == NULL) {
        fail(name, "complete json content wrong");
        return;
    }

    len = sidecar_build_json(buf, sizeof(buf), "x.pcm", &meta, false, 0, 0);
    if (len <= 0 || strstr(buf, "total_bytes") != NULL || strstr(buf, "\"complete\": false") == NULL) {
        fail(name, "partial json content wrong");
        return;
    }

    if (sidecar_build_json(buf, 16, "x.pcm", &meta, true, 1, 1) != -1) {
        fail(name, "small buffer accepted");
        return;
    }

    meta.format.sample_format = DUMP_PCM_FORMAT_UNKNOWN;
    if (sidecar_build_json(buf, sizeof(buf), "x.pcm", &meta, true, 1, 1) != -1) {
        fail(name, "unknown format accepted");
        return;
    }

    meta.format.sample_format = DUMP_PCM_FORMAT_S16_LE;
    meta.key1[0] = (char)0xC0;
    meta.key1[1] = (char)0xAF;
    meta.key1[2] = '\0';
    if (sidecar_build_json(buf, sizeof(buf), "x.pcm", &meta, true, 1, 1) != -1) {
        fail(name, "invalid utf8 key accepted");
        return;
    }

    pass(name);
}

static void fill_sidecar_meta(dump_pcm_file_meta_t *meta) {
    memset(meta, 0, sizeof(*meta));
    snprintf(meta->key1, sizeof(meta->key1), "out_primary");
    snprintf(meta->key2, sizeof(meta->key2), "music");
    meta->generation = 1;
    meta->split_index = 0;
    meta->format.sample_rate = 48000;
    meta->format.channel_count = 2;
    meta->format.bits_per_sample = 16;
    meta->format.sample_format = DUMP_PCM_FORMAT_S16_LE;
    meta->start_time_unix_ms = 1776397812123ULL;
}

static void test_sidecar_write_meta_file(void) {
    const char *name = "sidecar write meta file";
    const char *why = NULL;
    dump_pcm_file_meta_t meta;
    char pcm_path[512];
    char json_path[512];
    char buf[2048];
    int n = 0;

    reset_test_dir();
    snprintf(pcm_path, sizeof(pcm_path), "%ssidecar_case.pcm", file_mgr_get_root_dir());
    snprintf(json_path, sizeof(json_path), "%ssidecar_case.json", file_mgr_get_root_dir());
    fill_sidecar_meta(&meta);

    if (dump_pcm_set_metadata_enabled(1) != 0 || !sidecar_is_enabled()) {
        why = "public switch did not enable sidecar";
        goto done;
    }
    if (sidecar_write_meta(pcm_path, &meta, true, 1024, sidecar_now_unix_ms()) != 0) {
        why = "complete write failed";
        goto done;
    }

    n = read_file(json_path, buf, sizeof(buf) - 1);
    if (n <= 0) {
        why = "complete json not readable";
        goto done;
    }
    buf[n] = '\0';
    if (strstr(buf, "\"total_bytes\": 1024") == NULL || strstr(buf, "\"complete\": true") == NULL) {
        why = "complete json content wrong";
        goto done;
    }

    if (sidecar_write_meta(pcm_path, &meta, false, 0, 0) != 0) {
        why = "partial rewrite failed";
        goto done;
    }

    n = read_file(json_path, buf, sizeof(buf) - 1);
    if (n <= 0) {
        why = "rewritten json not readable";
        goto done;
    }
    buf[n] = '\0';
    if (strstr(buf, "total_bytes") != NULL || strstr(buf, "\"complete\": false") == NULL) {
        why = "rewritten json was not overwritten";
        goto done;
    }

done:
    remove(json_path);
    sidecar_set_enabled(0);
    if (why != NULL) {
        fail(name, why);
        return;
    }
    pass(name);
}

static void test_sidecar_switch_state(void) {
    const char *name = "sidecar switch state";

    sidecar_set_enabled(1);
    if (!sidecar_is_enabled()) {
        fail(name, "enable did not stick");
        return;
    }
    sidecar_set_enabled(0);
    if (sidecar_is_enabled()) {
        fail(name, "disable did not stick");
        return;
    }
    if (dump_pcm_set_metadata_enabled(1) != 0 || !sidecar_is_enabled()) {
        fail(name, "public switch did not enable");
        return;
    }
    if (dump_pcm_set_metadata_enabled(0) != 0 || sidecar_is_enabled()) {
        fail(name, "public switch did not disable");
        return;
    }

    pass(name);
}

static void test_sidecar_write_file_failure(void) {
    const char *name = "sidecar write file failure";

    if (sidecar_write_file("/nonexistent_audio_dump/x.json", "{}", 2) != -1) {
        fail(name, "unwritable directory accepted");
        return;
    }

    pass(name);
}

static void test_sidecar_lifecycle_on_real_file(void) {
    const char *name = "sidecar lifecycle on real file";
    const char *why = NULL;
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};
    char files[4][512] = {{0}};
    char json_path[512] = {0};
    char json[2048];
    uint64_t pcm_size = 0;
    uint64_t total_bytes = 0;
    uint64_t sample_rate = 0;
    int n = 0;

    reset_test_dir();
    if (dump_pcm_set_metadata_enabled(1) != 0 || !sidecar_is_enabled()) {
        why = "switch did not enable sidecar";
        goto done;
    }

    dump_pcm_data_write_ex("abcd", 4, "life", "cycle", &fmt);
    stop_and_wait("life", "cycle");

    if (find_pcm_files(files, 4) != 1) {
        why = "expected exactly one pcm file";
        goto done;
    }
    json_path_for_pcm(files[0], json_path, sizeof(json_path));
    if (!wait_for_sidecar(json_path, 2000)) {
        why = "sidecar missing after close";
        goto done;
    }
    if (file_size(files[0], &pcm_size) != 0) {
        why = "pcm size unavailable";
        goto done;
    }

    n = read_file(json_path, json, sizeof(json) - 1);
    if (n <= 0) {
        why = "sidecar unreadable";
        goto done;
    }
    json[n] = '\0';

    if (strstr(json, "\"complete\": true") == NULL) {
        why = "sidecar not marked complete";
        goto done;
    }
    if (!json_uint_field(json, "total_bytes", &total_bytes)) {
        why = "total_bytes missing";
        goto done;
    }
    if (total_bytes != pcm_size) {
        why = "total_bytes does not match pcm size";
        goto done;
    }
    if (!json_uint_field(json, "sample_rate", &sample_rate) || sample_rate != 48000) {
        why = "sample_rate wrong";
        goto done;
    }

done:
    remove(json_path);
    dump_pcm_set_metadata_enabled(0);
    remove_case_artifacts();
    if (why != NULL) {
        fail(name, why);
        return;
    }
    pass(name);
}

static void test_sidecar_legacy_write_leaves_no_sidecar(void) {
    const char *name = "legacy write leaves no sidecar";
    const char *why = NULL;

    reset_test_dir();
    if (dump_pcm_set_metadata_enabled(1) != 0 || !sidecar_is_enabled()) {
        why = "switch did not enable sidecar";
        goto done;
    }

    dump_pcm_data_write("legacy", 6, "legacy", "nosidecar");
    stop_and_wait("legacy", "nosidecar");

    if (count_pcm_files() != 1) {
        why = "expected exactly one pcm file";
        goto done;
    }
    if (count_json_files() != 0) {
        why = "legacy write produced a sidecar";
        goto done;
    }

done:
    dump_pcm_set_metadata_enabled(0);
    remove_case_artifacts();
    if (why != NULL) {
        fail(name, why);
        return;
    }
    pass(name);
}

static void test_sidecar_disabled_metadata_leaves_no_sidecar(void) {
    const char *name = "disabled metadata leaves no sidecar";
    const char *why = NULL;
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};

    reset_test_dir();
    dump_pcm_set_metadata_enabled(0);
    if (sidecar_is_enabled()) {
        why = "switch did not disable sidecar";
        goto done;
    }

    dump_pcm_data_write_ex("off", 3, "off", "sidecar", &fmt);
    stop_and_wait("off", "sidecar");

    if (count_pcm_files() != 1) {
        why = "expected exactly one pcm file";
        goto done;
    }
    if (count_json_files() != 0) {
        why = "disabled metadata produced a sidecar";
        goto done;
    }

done:
    dump_pcm_set_metadata_enabled(0);
    remove_case_artifacts();
    if (why != NULL) {
        fail(name, why);
        return;
    }
    pass(name);
}

static void test_sidecar_split_creates_two_sidecars(void) {
    const char *name = "split creates two sidecars";
    const char *why = NULL;
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};
    char files[4][512] = {{0}};
    char json_path[512] = {0};
    char buf[1024] = {0};
    int count = 0;
    int n = 0;
    bool saw_zero = false;
    bool saw_one = false;

    reset_test_dir();
    if (dump_pcm_set_metadata_enabled(1) != 0) {
        why = "switch did not enable sidecar";
        goto done;
    }

    dump_pcm_set_file_size_limit_for_test(10);
    dump_pcm_data_write_ex("0123456789", 10, "split", "meta", &fmt);
    dump_pcm_data_write_ex("abcde", 5, "split", "meta", &fmt);
    stop_and_wait("split", "meta");

    count = find_pcm_files(files, 4);
    if (count != 2) {
        why = "expected two pcm files";
        goto done;
    }

    for (int i = 0; i < count; ++i) {
        json_path_for_pcm(files[i], json_path, sizeof(json_path));
        if (!wait_for_sidecar(json_path, 2000)) {
            why = "missing sidecar";
            goto done;
        }
        n = read_file(json_path, buf, sizeof(buf) - 1);
        if (n <= 0) {
            why = "unreadable sidecar";
            goto done;
        }
        buf[n] = '\0';
        if (strstr(buf, "\"complete\": true") == NULL) {
            why = "split sidecar not complete";
            goto done;
        }
        if (strstr(buf, "\"split_index\": 0") != NULL) {
            saw_zero = true;
        }
        if (strstr(buf, "\"split_index\": 1") != NULL) {
            saw_one = true;
        }
    }
    if (!saw_zero || !saw_one) {
        why = "split indexes not recorded";
        goto done;
    }

done:
    dump_pcm_set_file_size_limit_for_test(0);
    dump_pcm_set_metadata_enabled(0);
    remove_case_artifacts();
    if (why != NULL) {
        fail(name, why);
        return;
    }
    pass(name);
}

static void test_sidecar_rollover_creates_new_sidecar(void) {
    const char *name = "rollover creates a new sidecar";
    const char *why = NULL;
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};
    char files[4][512] = {{0}};
    char json_path[512] = {0};
    char buf[1024] = {0};
    uint64_t gens[4] = {0};
    int count = 0;
    int n = 0;

    reset_test_dir();
    if (dump_pcm_set_metadata_enabled(1) != 0) {
        why = "switch did not enable sidecar";
        goto done;
    }

    dump_pcm_data_write_ex("aaaaa", 5, "roll", "meta", &fmt);
    usleep(2200000);
    dump_pcm_data_write_ex("bbbbb", 5, "roll", "meta", &fmt);
    stop_and_wait("roll", "meta");

    count = find_pcm_files(files, 4);
    if (count != 2) {
        why = "expected a new file after the rollover window";
        goto done;
    }

    for (int i = 0; i < count; ++i) {
        json_path_for_pcm(files[i], json_path, sizeof(json_path));
        if (!wait_for_sidecar(json_path, 2000)) {
            why = "missing sidecar";
            goto done;
        }
        n = read_file(json_path, buf, sizeof(buf) - 1);
        if (n <= 0) {
            why = "unreadable sidecar";
            goto done;
        }
        buf[n] = '\0';
        if (strstr(buf, "\"complete\": true") == NULL) {
            why = "rollover sidecar not complete";
            goto done;
        }
        if (!json_uint_field(buf, "generation", &gens[i])) {
            why = "generation missing from sidecar";
            goto done;
        }
    }
    if (gens[0] == gens[1]) {
        why = "rollover reused the generation";
        goto done;
    }

done:
    dump_pcm_set_metadata_enabled(0);
    remove_case_artifacts();
    if (why != NULL) {
        fail(name, why);
        return;
    }
    pass(name);
}

static int run_isolated_sidecar_kill_test(void) {
    const char *dir = getenv("AUDIO_DUMP_SIDECAR_KILL_DIR");
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};

    if (dir == NULL) {
        return 1;
    }
    if (dump_pcm_set_root_dir(dir) != 0 || dump_pcm_set_metadata_enabled(1) != 0) {
        return 1;
    }

    dump_pcm_data_write_ex("abcdef", 6, "kill", "meta", &fmt);
    usleep(250000);
    _exit(0); /* no stop(): the file is never finalised */
}

static void test_sidecar_survives_without_stop(void) {
    const char *name = "sidecar stays incomplete without stop";
    const char *why = NULL;
    char dir[256] = {0};
    char path[512] = {0};
    char buf[2048] = {0};
    int json_count = 0;
    int n = 0;
    char kill_env[384] = {0};
    const char *env[3];
    iso_result_t result;

    snprintf(dir, sizeof(dir), "%s/kill_%d/", tp_test_root(), (int)getpid());
    rmdir(dir);
    if (tp_mkdir(dir) != 0 && errno != EEXIST) {
        fail(name, "cannot prepare directory");
        return;
    }

    snprintf(kill_env, sizeof(kill_env), "AUDIO_DUMP_SIDECAR_KILL_DIR=%s", dir);
    env[0] = kill_env;
    env[1] = "AUDIO_DUMP_RUN_SIDECAR_KILL_TEST=1";
    env[2] = NULL;

    if (iso_run(env, &result) != 0 || !iso_ok(&result)) {
        why = "child did not exit cleanly";
        goto done;
    }

    {
        DIR *d = opendir(dir);
        struct dirent *entry = NULL;

        if (d == NULL) {
            why = "cannot scan directory";
            goto done;
        }
        while ((entry = readdir(d)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len > 5 && strcmp(entry->d_name + len - 5, ".json") == 0) {
                json_count++;
                snprintf(path, sizeof(path), "%s%s", dir, entry->d_name);
            }
        }
        closedir(d);
    }

    if (json_count != 1) {
        why = "expected exactly one sidecar";
        goto done;
    }
    n = read_file(path, buf, sizeof(buf) - 1);
    if (n <= 0) {
        why = "sidecar unreadable";
        goto done;
    }
    buf[n] = '\0';
    if (strstr(buf, "\"complete\": false") == NULL || strstr(buf, "total_bytes") != NULL) {
        why = "unfinalised sidecar looks complete";
        goto done;
    }

done:
    {
        DIR *d = opendir(dir);
        struct dirent *entry = NULL;

        if (d != NULL) {
            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                snprintf(path, sizeof(path), "%s%s", dir, entry->d_name);
                remove(path);
            }
            closedir(d);
        }
    }
    rmdir(dir);
    if (why != NULL) {
        fail(name, why);
        return;
    }
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
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_PUBLIC_WRITE_ALLOC_TEST=1", NULL};

    run_isolated_case(name, env, "isolated public write allocation failure test failed");
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
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_QUEUE_POP_TEST=1", NULL};

    run_isolated_case(name, env, "isolated queue_pop failure test failed");
}

static void test_writer_queue_full_write_rejection(void) {
    const char *name = "writer queue full write rejection";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_QUEUE_FULL_TEST=1", NULL};

    run_isolated_case(name, env, "isolated queue_full write rejection test failed");
}

static void test_writer_queue_full_failed_enqueue_keeps_rollover_clock(void) {
    const char *name = "writer queue full failed enqueue keeps rollover clock";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_QUEUE_FULL_CLOCK_TEST=1", NULL};

    run_isolated_case(name, env, "isolated queue_full clock test failed");
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

    /* Backfill the idle stamp instead of sleeping: the scan compares against
     * the stamp, so an epoch stamp is idle for any IDLE_TIMEOUT_SEC. The point
     * of the case is that the scan skips the *inactive* slot and evicts the
     * in-use idle one, which only an ownership assertion can see. */
    mgr.files[0].last_write_time = 0;
    mgr.count = MAX_OPEN_FILES;
    mgr.files[1].in_use = false;

    next = file_mgr_get_handle(&mgr, "idle", "b", 2, 0, path_b, sizeof(path_b), &is_new);
    if (next == NULL || mgr.count != MAX_OPEN_FILES) {
        file_mgr_destroy(&mgr);
        fail(name, "idle scan did not reuse slot cleanly");
        return;
    }

    /* The evicted slot must be the in-use idle one (key a), not the inactive
     * slot the scan is supposed to skip: closing the wrong slot leaves the
     * old path tracked and count would still read MAX_OPEN_FILES. */
    if (any_slot_tracks_path(&mgr, path_a)) {
        file_mgr_destroy(&mgr);
        fail(name, "idle scan closed the inactive slot instead of the idle one");
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
    static const char *const env[] = {"AUDIO_DUMP_ENABLED=1",
                                      "AUDIO_DUMP_RUN_SESSION_CAP_TEST=1", NULL};

    run_isolated_case(name, env, "expected session cap to stay fixed after overflow write");
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

    /* The accepted blocks must be on disk: a stop() that discards the pending
     * queue drains the counters just as cleanly, and only the byte count can
     * tell the two apart. The queue holds MAX_QUEUE_BLOCKS blocks while the
     * writer is frozen, so exactly that many 7-byte blocks should survive. */
    {
        char paths[4][512] = {{0}};
        uint64_t size = 0;

        if (find_pcm_files(paths, 4) != 1 || file_size(paths[0], &size) != 0 ||
            size != (uint64_t)MAX_QUEUE_BLOCKS * 7) {
            return 5;
        }
    }

    return 0;
}

static void test_stop_while_pending_blocks(void) {
    const char *name = "stop while pending blocks";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_STOP_PENDING_TEST=1", NULL};

    run_isolated_case(name, env, "isolated pending stop test failed");
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

    /* A buffer that cannot hold the whole name must be refused. Truncating
     * instead would drop the .pcm suffix and the hash, producing a file the
     * retention sweep never sees and another session can collide with. */
    {
        char tiny[40] = {0};

        if (file_mgr_get_handle(&mgr, "port", "tiny", 1, 0, tiny, sizeof(tiny), &is_new) != NULL) {
            file_mgr_close(&mgr, fp_a);
            file_mgr_close(&mgr, fp_b);
            file_mgr_destroy(&mgr);
            fail(name, "a buffer too small for the name was accepted");
            return;
        }
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

    /* A long root plus maximum-length keys must still yield a name that ends in
     * .pcm and stays unique. The identifying part (hash, generation, split,
     * timestamp) is what retention and collision-freedom depend on, so it is
     * the readable key part that has to give way when space runs out. */
    {
        char long_root[256] = {0};
        char long_key1[DUMP_PCM_KEY_MAX_LEN + 1];
        char long_key2[DUMP_PCM_KEY_MAX_LEN + 1];
        char long_a[256] = {0};
        char long_b[256] = {0};

        snprintf(long_root, sizeof(long_root), "%s/long_%0*d/", tp_test_root(), 120, 0);
        memset(long_key1, 'k', sizeof(long_key1) - 1);
        long_key1[sizeof(long_key1) - 1] = '\0';
        memset(long_key2, 'j', sizeof(long_key2) - 1);
        long_key2[sizeof(long_key2) - 1] = '\0';

        file_mgr_set_root_dir(long_root);
        if (file_mgr_init(&mgr) != 0) {
            file_mgr_set_root_dir(tp_test_root());
            fail(name, "file manager init failed under a long root");
            return;
        }

        fp_a = file_mgr_get_handle(&mgr, long_key1, long_key2, 1, 0, long_a, sizeof(long_a),
                                   &is_new);
        fp_b = file_mgr_get_handle(&mgr, long_key1, long_key2, 2, 0, long_b, sizeof(long_b),
                                   &is_new);
        if (fp_a == NULL || fp_b == NULL) {
            file_mgr_set_root_dir(tp_test_root());
            fail(name, "long root produced no file name at all");
            return;
        }
        if (strlen(long_a) + 1 > sizeof(long_a) || strlen(long_a) < 4 ||
            strcmp(long_a + strlen(long_a) - 4, ".pcm") != 0 ||
            strlen(long_b) < 4 || strcmp(long_b + strlen(long_b) - 4, ".pcm") != 0) {
            file_mgr_set_root_dir(tp_test_root());
            fail(name, "long root produced a name retention cannot recognise");
            return;
        }
        if (strcmp(long_a, long_b) == 0) {
            file_mgr_set_root_dir(tp_test_root());
            fail(name, "two generations collapsed onto one long name");
            return;
        }

        file_mgr_close(&mgr, fp_a);
        file_mgr_close(&mgr, fp_b);
        file_mgr_destroy(&mgr);
        file_mgr_set_root_dir(tp_test_root());
    }

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
    char host_backup[512] = {0};
    FILE *fp = NULL;

    reset_test_dir();
    snprintf(old_pcm, sizeof(old_pcm), "%sold_keepcheck.pcm", file_mgr_get_root_dir());
    snprintf(fresh_pcm, sizeof(fresh_pcm), "%sfresh_keepcheck.pcm", file_mgr_get_root_dir());
    snprintf(note_txt, sizeof(note_txt), "%snotes.txt", file_mgr_get_root_dir());
    snprintf(dir_pcm, sizeof(dir_pcm), "%snested_dir.pcm", file_mgr_get_root_dir());
    snprintf(host_backup, sizeof(host_backup), "%smix.pcm.bak", file_mgr_get_root_dir());

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
    fp = fopen(host_backup, "wb");
    if (fp != NULL) {
        fclose(fp);
    }
    tp_mkdir(dir_pcm);
    touch_old_file(old_pcm);
    touch_old_file(note_txt);
    touch_old_file(host_backup);

    if (file_mgr_cleanup(1, 1) != 0 || path_exists(old_pcm) || !path_exists(fresh_pcm) ||
        !path_exists(note_txt) || !path_exists(dir_pcm) || !path_exists(host_backup)) {
        remove(old_pcm);
        remove(fresh_pcm);
        remove(note_txt);
        remove(host_backup);
        rmdir(dir_pcm);
        fail(name, "cleanup filtered the wrong paths");
        return;
    }

    remove(fresh_pcm);
    remove(note_txt);
    remove(host_backup);
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
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected cleanup failure for file path conflict");
        return;
    }
    if (file_mgr_init(&mgr) == 0) {
        remove(conflict_path);
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected init failure for file path conflict");
        return;
    }
    remove(conflict_path);
    file_mgr_set_root_dir(tp_test_root());

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

    /* Eviction must pick the stalest slot (a, opened first), not whichever
     * slot the loop reaches first: the reverse ordering is invisible in
     * mgr.count, which is all the old assertion looked at. */
    if (any_slot_tracks_path(&mgr, path_a) || !any_slot_tracks_path(&mgr, path_b)) {
        file_mgr_destroy(&mgr);
        fail(name, "idle eviction did not pick the stalest slot");
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
    char blocker[256] = {0};
    char nested[320] = {0};
    char gone[256] = {0};
    FILE *fp = NULL;

    /* A regular file used as a path component can never become a directory, so
     * both the intermediate and the final mkdir below fail on every platform.
     * The old fixture used /proc, which only means "unwritable" on POSIX. */
    snprintf(blocker, sizeof(blocker), "%s/mgr_blocker", tp_test_root());
    remove(blocker);
    fp = fopen(blocker, "wb");
    if (fp != NULL) {
        fclose(fp);
    }
    snprintf(nested, sizeof(nested), "%s/subdir", blocker);

    file_mgr_set_root_dir(nested);
    if (file_mgr_init(&mgr) == 0) {
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected intermediate mkdir failure");
        return;
    }

    file_mgr_set_root_dir(blocker);
    if (file_mgr_init(&mgr) == 0) {
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected final mkdir failure");
        return;
    }

    /* Init over a writable directory succeeds; removing that directory makes the
     * later open fail. This replaces "init over /proc succeeds but opening is
     * not permitted", which no platform can guarantee portably. */
    snprintf(gone, sizeof(gone), "%s/mgr_gone_%d/", tp_test_root(), (int)getpid());
    rmdir(gone);
    file_mgr_set_root_dir(gone);
    if (file_mgr_init(&mgr) != 0) {
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected init to succeed on a writable directory");
        return;
    }

    rmdir(gone);
    fp = file_mgr_get_handle(&mgr, "openfail", "one", 1, 0, path, sizeof(path), &is_new);
    if (fp != NULL) {
        file_mgr_close(&mgr, fp);
        file_mgr_destroy(&mgr);
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected open to fail after the root directory vanished");
        return;
    }

    file_mgr_destroy(&mgr);
    remove(blocker);
    file_mgr_set_root_dir(tp_test_root());
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

    snprintf(root_dir, sizeof(root_dir), "%s_newdir_%ld", tp_test_root(), (long)getpid());
    reset_test_dir();
    file_mgr_set_root_dir(root_dir);
    if (file_mgr_init(&mgr) != 0) {
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "expected init to create a new directory");
        return;
    }

    fp_a = file_mgr_get_handle(&mgr, "destroy", "a", 1, 0, path_a, sizeof(path_a), &is_new);
    fp_b = file_mgr_get_handle(&mgr, "destroy", "b", 2, 0, path_b, sizeof(path_b), &is_new);
    if (fp_a == NULL || fp_b == NULL) {
        file_mgr_destroy(&mgr);
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "failed to open tracked handles");
        return;
    }

    file_mgr_destroy(&mgr);
    /* A leaked handle keeps the file undeletable on Windows, so a destroy that
     * forgets to close shows up here instead of being ignored. */
    if (remove(path_a) != 0 || remove(path_b) != 0) {
        file_mgr_set_root_dir(tp_test_root());
        fail(name, "destroy left a file handle open");
        return;
    }
    file_mgr_set_root_dir(tp_test_root());
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
    const char *dir = getenv("AUDIO_DUMP_DIR");
    dump_status_t status = {0};

    if (dir == NULL || dir[0] == '\0') {
        return 1;
    }

    /* The parent points this at a path whose parent component is a regular
     * file, so no platform can create it as a directory. */
    file_mgr_set_root_dir(dir);
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

static int run_isolated_runtime_controls_test(void) {
    dump_status_t status = {0};
    char dir[256] = {0};
    char other[256] = {0};

    snprintf(dir, sizeof(dir), "%s/rtc_%d/", tp_test_root(), (int)getpid());
    snprintf(other, sizeof(other), "%s/rtc_other_%d/", tp_test_root(), (int)getpid());
    rmdir(dir);
    rmdir(other);

    if (dump_pcm_set_root_dir(dir) != 0) {
        return 1;
    }

    /* The pushed switch must win over AUDIO_DUMP_ENABLED=1 from the parent. */
    dump_pcm_set_enabled(0);
    dump_pcm_data_write("x", 1, "rtc", "off");
    usleep(60000);
    if (path_exists(dir)) {
        return 1;
    }

    dump_pcm_set_enabled(1);
    dump_pcm_data_write("x", 1, "rtc", "on");
    stop_and_wait("rtc", "on");

    if (count_pcm_files() != 1) {
        return 1;
    }
    if (dump_pcm_data_get_status(&status) != 0 || !status.enabled) {
        return 1;
    }

    /* The root directory is frozen once the module is initialized. */
    if (dump_pcm_set_root_dir(other) != -1) {
        return 1;
    }
    if (strcmp(file_mgr_get_root_dir(), dir) != 0) {
        return 1;
    }

    return 0;
}

static int run_isolated_disabled_write_test(void) {
    dump_status_t status = {0};
    char dir[256] = {0};

    snprintf(dir, sizeof(dir), "%s/disabled_%d/", tp_test_root(), (int)getpid());
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
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_PTHREAD_CREATE_TEST=1", NULL};

    run_isolated_case(name, env, "isolated retry path failed");
}

static void test_init_failure_path(void) {
    const char *name = "init failure path";
    char conflict_dir[256] = {0};
    char blocker[256] = {0};
    char conflict_path[320] = {0};
    char dir_env[384] = {0};
    const char *env[4];
    iso_result_t result;
    FILE *fp = NULL;

    /* A path whose parent component is a regular file can never be created as
     * a directory. That proposition holds on every platform and drives the
     * same S_ISDIR branch in ensure_dir_exists() the old /dev/null/subdir
     * fixture relied on, without the POSIX device semantics. */
    snprintf(conflict_dir, sizeof(conflict_dir), "%s/init_fail_%d/", tp_test_root(),
             (int)getpid());
    rmdir(conflict_dir);
    (void)tp_mkdir(conflict_dir);
    snprintf(blocker, sizeof(blocker), "%sblocker", conflict_dir);
    fp = fopen(blocker, "wb");
    if (fp != NULL) {
        fclose(fp);
    }
    snprintf(conflict_path, sizeof(conflict_path), "%s/subdir", blocker);

    snprintf(dir_env, sizeof(dir_env), "AUDIO_DUMP_DIR=%s", conflict_path);
    env[0] = "AUDIO_DUMP_ENABLED=1";
    env[1] = dir_env;
    env[2] = "AUDIO_DUMP_RUN_INIT_FAILURE_TEST=1";
    env[3] = NULL;

    if (iso_run(env, &result) != 0 || !iso_ok(&result)) {
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
        char failpoint[64] = {0};
        const char *env[4];
        iso_result_t result;

        snprintf(failpoint, sizeof(failpoint), "AUDIO_DUMP_INIT_FAILPOINT=%s", modes[i]);
        env[0] = "AUDIO_DUMP_ENABLED=1";
        env[1] = "AUDIO_DUMP_RUN_INIT_FAILPOINT_TEST=1";
        env[2] = failpoint;
        env[3] = NULL;

        if (iso_run(env, &result) != 0 || !iso_ok(&result)) {
            fail(name, modes[i]);
            return;
        }
    }

    pass(name);
}

static void test_init_setclock_fallback(void) {
    const char *name = "init setclock fallback";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_SETCLOCK_FALLBACK_TEST=1", NULL};

    run_isolated_case(name, env, "isolated setclock fallback path failed");
}

static void test_disabled_write_has_no_side_effects(void) {
    const char *name = "disabled write has no side effects";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=0",
        "AUDIO_DUMP_RUN_DISABLED_WRITE_TEST=1", NULL};

    run_isolated_case(name, env, "disabled write produced init side effects");
}

static void test_cleanup_deferred_to_writer_thread(void) {
    const char *name = "cleanup deferred to writer thread";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_CLEANUP_DEFERRED_TEST=1", NULL};

    run_isolated_case(name, env, "caller thread performed a directory scan");
}

static void test_runtime_controls(void) {
    const char *name = "runtime switch and root directory";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_RUNTIME_CONTROLS_TEST=1", NULL};

    run_isolated_case(name, env, "runtime controls did not take effect");
}

static void test_init_wait_path(void) {
    const char *name = "init wait path";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_INIT_WAIT_TEST=1", NULL};

    run_isolated_case(name, env, "isolated init wait path failed");
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

    if (!g_init_wait_probe_started) {
        fprintf(stderr, "init_wait: the writer thread was never created through the hook\n");
        return 2;
    }

    /* stop_and_wait's fixed 120 ms is a courtesy to the writer thread, not a
     * guarantee. Under ASan and at -O2 on a loaded runner the teardown can
     * still be in flight here, so poll for the condition this case is actually
     * asserting instead of waiting a fixed time for it. */
    {
        int rc = -1;
        bool drained = false;

        for (int i = 0; i < 2000; ++i) {
            rc = dump_pcm_data_get_status(&status);
            if (rc == 0 && status.active_session_count == 0) {
                drained = true;
                break;
            }
            usleep(1000);
        }
        if (!drained) {
            fprintf(stderr, "init_wait: not drained after 2 s (rc=%d, sessions=%u)\n",
                    rc, status.active_session_count);
            return 1;
        }
    }

    /* The wait must not lose the block that triggered the slow init. The probe
     * thread is spawned by the create-hook, so it races the triggering write for
     * the queue slot and nothing in the module orders the two: the file holds
     * both payloads, in whichever order they landed. Asserting a fixed order
     * here is what made this case fail under ASan and at -O2 while passing at
     * -O0: the assertion was on the scheduler, not on the module. */
    {
        char paths[4][512] = {{0}};
        char content[32] = {0};
        int files = 0;
        int got = 0;

        files = find_pcm_files(paths, 4);
        if (files != 1) {
            fprintf(stderr, "init_wait: expected one file, found %d\n", files);
            return 3;
        }
        got = read_file(paths[0], content, sizeof(content) - 1);
        if (got != 9) {
            fprintf(stderr, "init_wait: expected 9 bytes, got %d\n", got);
            return 3;
        }
        if (strstr(content, "main") == NULL || strstr(content, "probe") == NULL) {
            fprintf(stderr, "init_wait: payloads missing, got '%s'\n", content);
            return 3;
        }
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

static void set_file_age(const char *path, long seconds) {
    struct utimbuf times;

    times.actime = time(NULL) - seconds;
    times.modtime = times.actime;
    utime(path, &times);
}

static int count_files_beginning_with(const char *prefix) {
    DIR *dir = opendir(file_mgr_get_root_dir());
    struct dirent *entry = NULL;
    size_t prefix_len = strlen(prefix);
    int count = 0;

    if (dir == NULL) {
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* An audio HAL is a long-lived process. Applying the retention bound only when
 * the writer thread starts means files that cross the seven-day line while the
 * process runs are never pruned. */
static void test_retention_reapplies_within_one_process(void) {
    const char *name = "retention re-applies within one process";
    const long past_the_window = 8L * 24L * 3600L;
    char path[512] = {0};
    char key2[16] = {0};

    reset_test_dir();
    /* Initialises the module: the sweep triggered here has nothing to prune, and
     * everything below happens in the same process. */
    dump_pcm_data_write("x", 1, "ret", "seed");
    stop_and_wait("ret", "seed");

    for (int i = 0; i < 105; ++i) {
        FILE *fp = NULL;

        snprintf(path, sizeof(path), "%sold_%03d.pcm", file_mgr_get_root_dir(), i);
        fp = fopen(path, "wb");
        if (fp == NULL) {
            fail(name, "could not plant an old file");
            return;
        }
        fclose(fp);
        set_file_age(path, past_the_window);
    }

    if (count_files_beginning_with("old_") != 105) {
        fail(name, "planted files are not all present");
        return;
    }

    /* The sweep is driven by completed sessions, so a bounded number of them
     * has to reach it. Eight is the writer's interval. */
    for (int i = 0; i < 8; ++i) {
        snprintf(key2, sizeof(key2), "s%d", i);
        dump_pcm_data_write("x", 1, "ret", key2);
        stop_and_wait("ret", key2);
    }
    /* Let the writer thread wake, notice, and sweep. */
    usleep(300000);

    if (count_files_beginning_with("old_") > 100) {
        fail(name, "old files survived a whole process lifetime");
        return;
    }

    pass(name);
}

/* A table rather than a call list: the harness can then assert on the count,
 * and dropping a case during a refactor becomes a build error instead of a
 * quietly shorter run. */
typedef struct {
    const char *name;
    void (*run)(void);
} test_case_t;

/* Discrimination control for the audit. Manufactures one real F->C edge and
 * requires the detector to report exactly one. C is free at that moment, so
 * this can never deadlock and needs no barrier or timeout. If this case fails,
 * the production-path case would be vacuous. */
static int run_isolated_lock_order_negative(void) {
    pthread_mutex_t *c_lock = &g_ctx.mutex;
    pthread_mutex_t *f_lock = (pthread_mutex_t *)file_mgr_mutex_addr_for_test();

    if (pthread_mutex_init(c_lock, NULL) != 0) {
        return 2;
    }
    lock_audit_reset();
    if (pthread_mutex_lock(f_lock) != 0) {
        return 3;
    }
    if (pthread_mutex_lock(c_lock) != 0) {
        pthread_mutex_unlock(f_lock);
        return 4;
    }
    pthread_mutex_unlock(c_lock);
    pthread_mutex_unlock(f_lock);
    atomic_store(&g_lock_audit_active, 0);

    if (atomic_load(&g_f_to_c_edges) != 1) {
        return 5;
    }
    if (atomic_load(&g_f_takes) < 1 || atomic_load(&g_c_takes) < 1) {
        return 6;
    }
    pthread_mutex_destroy(c_lock);
    return 0;
}

/* Production-path case: drive decision / execution / publish with a real
 * write + stop (format enabled, so both the PCM and the sidecar I/O paths run),
 * then assert that no C acquisition happened while this thread held F.
 * The bounded wait is fixture setup, not the judgement; the assertion is on
 * lock ownership only. */
static int run_isolated_lock_order_positive(void) {
    dump_pcm_format_t fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};
    dump_status_t status = {0};
    int i = 0;

    reset_test_dir();
    tp_setenv("AUDIO_DUMP_ENABLED", "1");
    logger_init(pc_log_fn);
    logger_set_level(LOG_LEVEL_INFO);
    dump_pcm_set_metadata_enabled(1);

    lock_audit_reset();
    dump_pcm_data_write_ex("stage-two-lock-order-probe", 25, "lock", "order", &fmt);
    for (i = 0; i < 500; ++i) {
        if (dump_pcm_data_get_status(&status) == 0 &&
            status.queue_pending_blocks == 0) {
            break;
        }
        usleep(1000);
    }
    dump_pcm_data_stop("lock", "order");
    usleep(100000);
    atomic_store(&g_lock_audit_active, 0);

    if (atomic_load(&g_f_takes) < 2) {
        return 2; /* the audit never saw the file-manager path */
    }
    if (atomic_load(&g_c_takes) < 3) {
        return 3; /* decision + publish + stop never ran */
    }
    if (atomic_load(&g_f_to_c_edges) != 0) {
        return 4; /* L1 / L2 violated on the production path */
    }
    return 0;
}

static void test_lock_order_negative_control(void) {
    const char *name = "lock order audit detects a manufactured F->C edge";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_LOCK_ORDER_NEGATIVE_TEST=1", NULL};

    run_isolated_case(name, env, "audit did not detect the manufactured F->C edge");
}

static void test_lock_order_production_path(void) {
    const char *name = "three-phase writer takes C only after releasing F";
    static const char *const env[] = {
        "AUDIO_DUMP_ENABLED=1",
        "AUDIO_DUMP_RUN_LOCK_ORDER_POSITIVE_TEST=1", NULL};

    run_isolated_case(name, env, "production path produced an F->C edge");
}

/* plan 2.2.3: the real split threshold is 1 GB, which no fixture can cross.
 * The injected limit drives the same close-and-reopen path through the public
 * API. Every chunk is one repeated byte, so a lost or duplicated block shows
 * up in the per-byte counts even though shard order is not asserted. */
static void test_file_split_via_public_api(void) {
    const char *name = "file split via public api";
    static char chunk[2000];
    char paths[8][512] = {{0}};
    char content[16384] = {0};
    int counts[6] = {0};
    int files = 0;
    int total = 0;
    const int chunks = 6;

    reset_test_dir();
    dump_pcm_set_file_size_limit_for_test(4096);
    for (int i = 0; i < chunks; ++i) {
        memset(chunk, 'A' + i, sizeof(chunk));
        dump_pcm_data_write(chunk, sizeof(chunk), "split", "public");
    }
    stop_and_wait("split", "public");
    dump_pcm_set_file_size_limit_for_test(0);

    files = find_pcm_files(paths, 8);
    if (files < 2) {
        fail(name, "the injected limit must force more than one shard");
        return;
    }

    for (int i = 0; i < files; ++i) {
        uint64_t size = 0;
        int got = 0;

        /* The writer decides before each block, so a shard may overshoot the
         * limit by at most one block; anything past that means the limit was
         * never applied. */
        if (file_size(paths[i], &size) != 0 || size > 4096 + sizeof(chunk)) {
            fail(name, "a shard ran past the injected limit");
            return;
        }
        if ((size_t)total + size >= sizeof(content)) {
            fail(name, "the shards overflow the readback buffer");
            return;
        }
        got = read_file(paths[i], content + total, sizeof(content) - (size_t)total - 1);
        if (got != (int)size) {
            fail(name, "a shard could not be read back whole");
            return;
        }
        total += got;
    }

    if (total != chunks * (int)sizeof(chunk)) {
        fail(name, "the shards do not add up to the payload");
        return;
    }

    for (int i = 0; i < total; ++i) {
        int slot = content[i] - 'A';
        if (slot < 0 || slot >= chunks) {
            fail(name, "a shard holds a byte that was never written");
            return;
        }
        counts[slot]++;
    }
    for (int i = 0; i < chunks; ++i) {
        if (counts[i] != (int)sizeof(chunk)) {
            fail(name, "a shard lost or duplicated bytes");
            return;
        }
    }

    pass(name);
}

/* plan 2.2.6: the idle timeout releases a handle, never a session. The scan only
 * runs when the handle table is full, so the case needs two live sessions and a
 * third write. The idle stamps are backfilled instead of slept on; this is the
 * one sanctioned place where a case reaches into the module's state, because
 * the scan has no injectable clock and a real sleep would prove only the PC
 * value (1 s), never the production one (5 s). */
static void test_idle_timeout_keeps_session_and_file(void) {
    const char *name = "idle timeout keeps session and file";
    dump_status_t status = {0};
    int dropped_before = 0;
    int dropped_after = 0;

    reset_test_dir();
    if (dump_pcm_data_get_status(&status) != 0) {
        fail(name, "status unavailable before the case");
        return;
    }
    dropped_before = status.dropped_blocks;

    dump_pcm_data_write("aaaaa", 5, "idle_survive", "keep");
    dump_pcm_data_write("bbbbb", 5, "idle_survive", "other");
    usleep(300000);

    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (g_ctx.file_mgr.files[i].in_use) {
            g_ctx.file_mgr.files[i].last_write_time = 0;
        }
    }
    if (g_ctx.file_mgr.count != MAX_OPEN_FILES) {
        fail(name, "both sessions must occupy a handle before the scan");
        return;
    }

    dump_pcm_data_write("ccccc", 5, "idle_survive", "third");
    usleep(300000);

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 3) {
        fail(name, "the idle scan must release a handle, not a session");
        return;
    }
    if (g_ctx.file_mgr.count != MAX_OPEN_FILES) {
        fail(name, "the idle scan neither evicted nor reopened a slot");
        return;
    }
    if (count_pcm_files() != 3) {
        fail(name, "the third session must own a file of its own");
        return;
    }

    /* The evicted session keeps its file: a later write reopens the same path
     * instead of starting a new session or a new file. The table is full with
     * B and C, neither idle, so make a slot available first. */
    for (uint32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (g_ctx.file_mgr.files[i].in_use) {
            g_ctx.file_mgr.files[i].last_write_time = 0;
        }
    }
    dump_pcm_data_write("ddddd", 5, "idle_survive", "keep");
    usleep(300000);

    if (dump_pcm_data_get_status(&status) != 0 || status.active_session_count != 3) {
        fail(name, "writing an evicted session must not open a new session");
        return;
    }
    if (count_pcm_files() != 3) {
        fail(name, "the reopened session must reuse its file");
        return;
    }

    dropped_after = status.dropped_blocks;
    if (dropped_after != dropped_before) {
        fail(name, "a block was dropped while a handle was being reclaimed");
        return;
    }

    pass(name);
}

typedef struct {
    pthread_barrier_t *barrier;
    char byte;
} concurrent_payload_args_t;

static void *concurrent_payload_write(void *arg) {
    const concurrent_payload_args_t *args = (const concurrent_payload_args_t *)arg;
    char block[64];

    memset(block, args->byte, sizeof(block));
    pthread_barrier_wait(args->barrier);
    for (int i = 0; i < 4; ++i) {
        dump_pcm_data_write(block, sizeof(block), "concurrent", "loss");
    }
    return NULL;
}

/* plan 2.1.6: two threads racing into write() must lose nothing and duplicate
 * nothing. The writer is frozen first, so the assertion can separate "queued"
 * from "landed": while frozen the queue must hold every accepted block and the
 * disk must hold none. The payloads are distinct bytes, so a lost block, a
 * duplicated block and an interleaved rewrite are all distinguishable. */
static int run_isolated_concurrent_write(void) {
    pthread_barrier_t barrier;
    pthread_t threads[2] = {0};
    concurrent_payload_args_t args[2] = {{0}};
    dump_status_t status = {0};
    char paths[4][512] = {{0}};
    char content[1024] = {0};
    int got = 0;
    int count_a = 0;
    int count_b = 0;

    reset_test_dir();
    atomic_store(&g_writer_hold_ready, 0);
    atomic_store(&g_writer_hold_release, 0);
    dump_pcm_set_pthread_create_fn_for_test(fake_pthread_create_hold_writer);

    if (pthread_barrier_init(&barrier, NULL, 3) != 0) {
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 1;
    }

    args[0].barrier = &barrier;
    args[0].byte = 'A';
    args[1].barrier = &barrier;
    args[1].byte = 'B';

    if (pthread_create(&threads[0], NULL, concurrent_payload_write, &args[0]) != 0 ||
        pthread_create(&threads[1], NULL, concurrent_payload_write, &args[1]) != 0) {
        pthread_barrier_destroy(&barrier);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 1;
    }

    pthread_barrier_wait(&barrier);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    pthread_barrier_destroy(&barrier);

    if (!wait_for_writer_hold_ready()) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 2;
    }

    if (dump_pcm_data_get_status(&status) != 0 ||
        status.queue_pending_blocks != MAX_QUEUE_BLOCKS) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 3;
    }
    if (count_pcm_files() != 0) {
        atomic_store(&g_writer_hold_release, 1);
        dump_pcm_set_pthread_create_fn_for_test(NULL);
        return 4;
    }

    atomic_store(&g_writer_hold_release, 1);
    dump_pcm_set_pthread_create_fn_for_test(NULL);
    usleep(200000);
    stop_and_wait("concurrent", "loss");

    if (find_pcm_files(paths, 4) != 1) {
        return 5;
    }

    got = read_file(paths[0], content, sizeof(content) - 1);
    if (got != 512) {
        return 6;
    }
    for (int i = 0; i < got; ++i) {
        if (content[i] == 'A') {
            count_a++;
        } else if (content[i] == 'B') {
            count_b++;
        }
    }
    if (count_a != 256 || count_b != 256) {
        return 7;
    }

    return 0;
}

static void test_concurrent_write_no_loss(void) {
    const char *name = "concurrent write no loss";
    static const char *const env[] = {"AUDIO_DUMP_ENABLED=1",
                                      "AUDIO_DUMP_RUN_CONCURRENT_WRITE_TEST=1", NULL};

    run_isolated_case(name, env, "isolated concurrent write failed");
}

static const test_case_t g_cases[] = {
    {"test_string_key_write_and_escape", test_string_key_write_and_escape},
    {"test_key_validation", test_key_validation},
    {"test_argument_guard_coverage", test_argument_guard_coverage},
    {"test_storage_cache_interval_hits_cached_path", test_storage_cache_interval_hits_cached_path},
    {"test_storage_cache_hits_cached_under_lock", test_storage_cache_hits_cached_under_lock},
    {"test_session_separation_and_query", test_session_separation_and_query},
    {"test_stop_then_new_session_creates_new_file", test_stop_then_new_session_creates_new_file},
    {"test_write_gap_exactly_two_seconds_keeps_same_file", test_write_gap_exactly_two_seconds_keeps_same_file},
    {"test_write_gap_over_two_seconds_creates_new_file", test_write_gap_over_two_seconds_creates_new_file},
    {"test_low_storage_latch_requires_reenable", test_low_storage_latch_requires_reenable},
    {"test_filename_hash_disambiguates_escaped_collisions", test_filename_hash_disambiguates_escaped_collisions},
    {"test_same_timestamp_rollovers_get_unique_files", test_same_timestamp_rollovers_get_unique_files},
    {"test_storage_failure_blocks_new_session", test_storage_failure_blocks_new_session},
    {"test_storage_low_storage_blocks_new_session", test_storage_low_storage_blocks_new_session},
    {"test_storage_large_storage_reports_full_value", test_storage_large_storage_reports_full_value},
    {"test_queue_direct", test_queue_direct},
    {"test_queue_empty_pop_and_byte_limit", test_queue_empty_pop_and_byte_limit},
    {"test_session_direct", test_session_direct},
    {"test_session_highest_generation_lookup", test_session_highest_generation_lookup},
    {"test_session_file_action_policy", test_session_file_action_policy},
    {"test_format_validation", test_format_validation},
    {"test_write_ex_captures_first_format", test_write_ex_captures_first_format},
    {"test_write_ex_without_format_matches_legacy", test_write_ex_without_format_matches_legacy},
    {"test_sidecar_utf8_validation", test_sidecar_utf8_validation},
    {"test_sidecar_json_builder", test_sidecar_json_builder},
    {"test_sidecar_write_meta_file", test_sidecar_write_meta_file},
    {"test_sidecar_switch_state", test_sidecar_switch_state},
    {"test_sidecar_write_file_failure", test_sidecar_write_file_failure},
    {"test_sidecar_lifecycle_on_real_file", test_sidecar_lifecycle_on_real_file},
    {"test_sidecar_legacy_write_leaves_no_sidecar", test_sidecar_legacy_write_leaves_no_sidecar},
    {"test_sidecar_disabled_metadata_leaves_no_sidecar", test_sidecar_disabled_metadata_leaves_no_sidecar},
    {"test_sidecar_split_creates_two_sidecars", test_sidecar_split_creates_two_sidecars},
    {"test_sidecar_rollover_creates_new_sidecar", test_sidecar_rollover_creates_new_sidecar},
    {"test_sidecar_survives_without_stop", test_sidecar_survives_without_stop},
    {"test_queue_allocation_failures", test_queue_allocation_failures},
    {"test_session_allocation_failures", test_session_allocation_failures},
    {"test_public_write_allocation_failures", test_public_write_allocation_failures},
    {"test_writer_queue_pop_failure", test_writer_queue_pop_failure},
    {"test_writer_queue_full_write_rejection", test_writer_queue_full_write_rejection},
    {"test_writer_queue_full_failed_enqueue_keeps_rollover_clock", test_writer_queue_full_failed_enqueue_keeps_rollover_clock},
    {"test_session_capacity_rejection", test_session_capacity_rejection},
    {"test_public_write_rejects_when_session_capacity_reached", test_public_write_rejects_when_session_capacity_reached},
    {"test_stop_while_pending_blocks", test_stop_while_pending_blocks},
    {"test_session_helper_direct", test_session_helper_direct},
    {"test_file_manager_direct", test_file_manager_direct},
    {"test_file_manager_reuses_tracked_handle", test_file_manager_reuses_tracked_handle},
    {"test_file_manager_closes_fp_when_no_free_slot", test_file_manager_closes_fp_when_no_free_slot},
    {"test_file_manager_idle_scan_skips_inactive_slots", test_file_manager_idle_scan_skips_inactive_slots},
    {"test_file_manager_rejects_when_full_and_no_idle_slot", test_file_manager_rejects_when_full_and_no_idle_slot},
    {"test_file_manager_close_rejects_foreign_handle", test_file_manager_close_rejects_foreign_handle},
    {"test_file_manager_cleanup_filters_non_pcm_files", test_file_manager_cleanup_filters_non_pcm_files},
    {"test_get_active_sessions_argument_guards", test_get_active_sessions_argument_guards},
    {"test_file_manager_cleanup_and_evict", test_file_manager_cleanup_and_evict},
    {"test_file_manager_init_and_open_failures", test_file_manager_init_and_open_failures},
    {"test_file_manager_init_creates_and_destroy_closes", test_file_manager_init_creates_and_destroy_closes},
    {"test_file_manager_cleanup_realloc_failure", test_file_manager_cleanup_realloc_failure},
    {"test_logger_direct", test_logger_direct},
    {"test_init_retry_path", test_init_retry_path},
    {"test_init_failure_path", test_init_failure_path},
    {"test_init_do_init_failures", test_init_do_init_failures},
    {"test_init_setclock_fallback", test_init_setclock_fallback},
    {"test_disabled_write_has_no_side_effects", test_disabled_write_has_no_side_effects},
    {"test_cleanup_deferred_to_writer_thread", test_cleanup_deferred_to_writer_thread},
    {"test_runtime_controls", test_runtime_controls},
    {"test_init_wait_path", test_init_wait_path},
    {"test_whitebox_coverage_shim", test_whitebox_coverage_shim},
    {"test_retention_reapplies_within_one_process",
     test_retention_reapplies_within_one_process},
    {"test_lock_order_negative_control", test_lock_order_negative_control},
    {"test_lock_order_production_path", test_lock_order_production_path},
    {"test_file_split_via_public_api", test_file_split_via_public_api},
    {"test_idle_timeout_keeps_session_and_file", test_idle_timeout_keeps_session_and_file},
    {"test_concurrent_write_no_loss", test_concurrent_write_no_loss},
};

_Static_assert(sizeof(g_cases) / sizeof(g_cases[0]) == 72,
               "the suite must register exactly 72 cases");

int main(int argc, char **argv) {
    const char *base_dir = NULL;

    iso_init((argc > 0) ? argv[0] : NULL);
    /* Line buffered even when stdout is a pipe: the suite may die mid-run. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Point the module at the suite's scratch root before anything else. The
     * isolated branches below return from main() before the parent's own setup,
     * so without this a child falls back to the module's compiled-in default
     * (/tmp/audio_dump_test) and writes outside the directory the parent
     * prepared - which also made those cases immune to AUDIO_DUMP_DIR. A
     * not-yet-initialised module accepts the root; an already-initialised one
     * ignores it, which is why this has to be first. */
    file_mgr_set_root_dir(tp_test_root());

    if (getenv("AUDIO_DUMP_RUN_LOCK_ORDER_NEGATIVE_TEST") != NULL) {
        return run_isolated_lock_order_negative();
    }

    if (getenv("AUDIO_DUMP_RUN_LOCK_ORDER_POSITIVE_TEST") != NULL) {
        return run_isolated_lock_order_positive();
    }

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

    if (getenv("AUDIO_DUMP_RUN_RUNTIME_CONTROLS_TEST") != NULL) {
        return run_isolated_runtime_controls_test();
    }

    if (getenv("AUDIO_DUMP_RUN_SIDECAR_KILL_TEST") != NULL) {
        return run_isolated_sidecar_kill_test();
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

    if (getenv("AUDIO_DUMP_RUN_CONCURRENT_WRITE_TEST") != NULL) {
        return run_isolated_concurrent_write();
    }

    if (getenv("AUDIO_DUMP_RUN_SESSION_CAP_TEST") != NULL) {
        return run_isolated_session_capacity_test();
    }

    if (getenv("AUDIO_DUMP_RUN_STOP_PENDING_TEST") != NULL) {
        return run_isolated_stop_while_pending_blocks();
    }

    logger_init(pc_log_fn);
    logger_set_level(LOG_LEVEL_INFO);
    base_dir = getenv("AUDIO_DUMP_DIR") != NULL ? getenv("AUDIO_DUMP_DIR") : tp_test_root();
    strncpy(g_test_dir_base, base_dir, sizeof(g_test_dir_base) - 1);
    g_test_dir_base[sizeof(g_test_dir_base) - 1] = '\0';
    file_mgr_set_root_dir(g_test_dir_base);
    tp_mkdir(file_mgr_get_root_dir());
    tp_setenv("AUDIO_DUMP_ENABLED", "1");

    printf("========================================\n");
    printf("   PCM Dump Module Test Suite\n");
    printf("========================================\n");

    for (size_t i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); ++i) {
        /* Name the case before running it and flush: a crash must not swallow
         * the only evidence of where it happened. */
        printf("RUN:  %s\n", g_cases[i].name);
        fflush(stdout);
        g_fixture_error = NULL;
        g_cases[i].run();
    }

    printf("\n========================================\n");
    printf("   Test Results\n");
    printf("========================================\n");
    printf("Tests Passed: %d\n", tests_passed);
    printf("Tests Failed: %d\n", tests_failed);
    printf("Total Tests:  %d\n", tests_passed + tests_failed);
    printf("Registered:   %zu\n", sizeof(g_cases) / sizeof(g_cases[0]));
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
