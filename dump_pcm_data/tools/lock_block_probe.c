/* Stage-2 G2 auxiliary probe: 20 ms blocking-injection amplification.
 *
 * Implements section 13.2 of docs/stage2-lock-order-spec.md. A slow fopen on
 * the writer thread must NOT be amplified onto the audio/caller thread, because
 * the three-phase writer executes all file I/O outside C. This is an auxiliary
 * criterion: the primary criterion is the deterministic lock-attribution cases
 * in test/main.c, which do not depend on OS timing.
 *
 * The probe links the production source files verbatim with three wrappers:
 *   --wrap=fopen                  -> inject delay by mode ("ab" PCM, "wb" sidecar)
 *   --wrap=pthread_mutex_lock    -> count F->C edges while F is held
 *   --wrap=pthread_mutex_unlock  -> maintain the per-thread F-held bit
 *
 * It is deliberately NOT linked into dump_pcm_test and does not touch the three
 * shared wrap lists. It is built with the production (default) platform config:
 * the PC config caps the queue at 8 blocks, which would turn a 20 ms writer
 * stall into throughput-driven drops unrelated to lock contention.
 *
 * Reproduce: sh tools/run_blocking_probe.sh
 * Results:   docs/stage2-lock-order-probe.md
 */
#define _POSIX_C_SOURCE 200809L
#define DUMP_PCM_INTERNAL_DEBUG_API 1

#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dump_internal.h"
#include "logger.h"

/* Section 13.2 constants. Keep these in step with test/check_blocking_probe.sh. */
#define SAMPLES 2000
#define WARMUP_SKIP 10
#define LIMIT_BASELINE_US 2000
#define LIMIT_INJECTED_US 10000
#define LIMIT_BLOCKED_US 10000
#define LIMIT_DROPPED_DELTA 20

static const int g_delays[4] = {0, 20, 0, 20};
static const char *const g_face_name[2] = {"pcm", "sidecar"};

/* The caller writes one block per millisecond so the 2000 samples really
 * overlap the writer's injected fopen stalls. A tight 2000-iteration loop
 * finishes before the writer opens a single file, which would silently turn
 * the injection into a no-op. Windows usleep() rounds short waits to the
 * ~15 ms scheduler tick, so throttle against CLOCK_MONOTONIC instead of
 * sleeping: a 1 ms request would otherwise stretch a group to ~30 s and
 * amplify the injection from tens of hits to 800+. */
#define CALLER_INTERVAL_US 1000
/* A 25 ms churn made the writer spend most of a group inside injected sidecar
 * opens, which fills the queue for throughput reasons unrelated to C. At
 * 100 ms the injection still covers the whole 2 s caller window (~20 hits)
 * without turning the probe into a writer-throughput benchmark. */
#define SESSION_INTERVAL_US 100000

static atomic_int g_stop;
static atomic_int g_delay_pcm_ms;
static atomic_int g_delay_sidecar_ms;
static atomic_uint g_pcm_hits;
static atomic_uint g_sidecar_hits;
static atomic_uint g_fc_edges;

static pthread_mutex_t *g_F_addr;
static pthread_mutex_t *g_C_addr;
static _Thread_local int t_f_held;

static const dump_pcm_format_t g_fmt = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};

FILE *__real_fopen(const char *path, const char *mode);
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);

FILE *__wrap_fopen(const char *path, const char *mode) {
    int delay = 0;

    if (mode != NULL && strcmp(mode, "ab") == 0) {
        delay = atomic_load(&g_delay_pcm_ms);
        if (delay > 0) {
            atomic_fetch_add(&g_pcm_hits, 1);
        }
    } else if (mode != NULL && strcmp(mode, "wb") == 0) {
        delay = atomic_load(&g_delay_sidecar_ms);
        if (delay > 0) {
            atomic_fetch_add(&g_sidecar_hits, 1);
        }
    }
    if (delay > 0) {
        usleep((useconds_t)delay * 1000);
    }
    return __real_fopen(path, mode);
}

/* A thread that takes C while still holding F would close the ABBA cycle.
 * Count those edges for the whole run; §13.2 requires zero. */
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    int rc;

    if (g_C_addr != NULL && mutex == g_C_addr && t_f_held) {
        atomic_fetch_add(&g_fc_edges, 1);
    }
    rc = __real_pthread_mutex_lock(mutex);
    if (g_F_addr != NULL && mutex == g_F_addr) {
        t_f_held = 1;
    }
    return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    int rc = __real_pthread_mutex_unlock(mutex);

    if (g_F_addr != NULL && mutex == g_F_addr) {
        t_f_held = 0;
    }
    return rc;
}

static uint64_t now_us(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void wait_until(uint64_t deadline_us) {
    /* clock_gettime() is the observable side effect; the loop cannot be
     * optimised away and does not depend on the OS sleep granularity. */
    while (now_us() < deadline_us) {
        /* spin */
    }
}

static void *caller_thread(void *arg) {
    uint64_t *samples = (uint64_t *)arg;
    int i;

    for (i = 0; i < SAMPLES; ++i) {
        uint64_t t0 = now_us();

        dump_pcm_data_write_ex("abcd", 4, "mixer", "out", &g_fmt);
        samples[i] = now_us() - t0;
        wait_until(t0 + CALLER_INTERVAL_US);
    }
    atomic_store(&g_stop, 1);
    return NULL;
}

/* Each fresh session forces one PCM fopen("ab") plus an initial/final sidecar
 * fopen("wb"), so both injection faces are exercised during the caller's run. */
static void *session_thread(void *arg) {
    char key2[32];
    int i = 0;

    (void)arg;
    while (!atomic_load(&g_stop)) {
        snprintf(key2, sizeof(key2), "take%04d", i++);
        dump_pcm_data_write_ex("abcd", 4, "mixer", key2, &g_fmt);
        usleep(SESSION_INTERVAL_US);
        dump_pcm_data_stop("mixer", key2);
    }
    return NULL;
}

typedef struct {
    int delay_ms;
    uint64_t caller_max_us;
    uint64_t over_10ms;
    uint64_t dropped;
    uint64_t pcm_hits;
    uint64_t sidecar_hits;
} group_result_t;

static int run_group(int face, int seq, int delay_ms, group_result_t *out) {
    pthread_t caller;
    pthread_t sessions;
    uint64_t *samples = calloc(SAMPLES, sizeof(*samples));
    dump_status_t s0 = {0};
    dump_status_t s1 = {0};
    uint64_t worst = 0;
    uint64_t over = 0;
    uint64_t pcm0;
    uint64_t sidecar0;
    int i;

    (void)seq;
    if (samples == NULL) {
        return -1;
    }

    atomic_store(&g_delay_pcm_ms, face == 0 ? delay_ms : 0);
    atomic_store(&g_delay_sidecar_ms, face == 1 ? delay_ms : 0);
    atomic_store(&g_stop, 0);
    pcm0 = atomic_load(&g_pcm_hits);
    sidecar0 = atomic_load(&g_sidecar_hits);
    if (dump_pcm_data_get_status(&s0) != 0) {
        free(samples);
        return -1;
    }

    if (pthread_create(&sessions, NULL, session_thread, NULL) != 0) {
        free(samples);
        return -1;
    }
    usleep(50000);
    if (pthread_create(&caller, NULL, caller_thread, samples) != 0) {
        atomic_store(&g_stop, 1);
        pthread_join(sessions, NULL);
        free(samples);
        return -1;
    }
    pthread_join(caller, NULL);
    pthread_join(sessions, NULL);

    for (i = WARMUP_SKIP; i < SAMPLES; ++i) {
        if (samples[i] > worst) {
            worst = samples[i];
        }
        if (samples[i] >= LIMIT_BLOCKED_US) {
            over++;
        }
    }
    if (dump_pcm_data_get_status(&s1) != 0) {
        free(samples);
        return -1;
    }

    out->delay_ms = delay_ms;
    out->caller_max_us = worst;
    out->over_10ms = over;
    out->dropped = s1.dropped_blocks - s0.dropped_blocks;
    out->pcm_hits = atomic_load(&g_pcm_hits) - pcm0;
    out->sidecar_hits = atomic_load(&g_sidecar_hits) - sidecar0;
    free(samples);

    /* Close the "out" session so the next group opens a fresh PCM file. Do it
     * after the counters are sampled and with the injection switched off. */
    atomic_store(&g_delay_pcm_ms, 0);
    atomic_store(&g_delay_sidecar_ms, 0);
    dump_pcm_data_stop("mixer", "out");
    return 0;
}

static uint64_t count_sidecars(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    uint64_t n = 0;

    if (d == NULL) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (len > 5 && strcmp(e->d_name + len - 5, ".json") == 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void print_group(const char *tag, int face, int seq, const group_result_t *r) {
    printf("[group] %s face=%s seq=%d delay=%dms caller_max=%lluus blocked>=10ms=%llu "
           "dropped=%llu pcm_hits=%llu sidecar_hits=%llu\n",
           tag, g_face_name[face], seq, r->delay_ms,
           (unsigned long long)r->caller_max_us, (unsigned long long)r->over_10ms,
           (unsigned long long)r->dropped, (unsigned long long)r->pcm_hits,
           (unsigned long long)r->sidecar_hits);
}

static void fail(int *count, const char *fmt, ...) {
    va_list ap;

    printf("[FAIL] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    (*count)++;
}

int main(void) {
    char root[256];
    group_result_t groups[2][4];
    group_result_t reverse;
    uint64_t sidecars_before;
    uint64_t sidecars_after;
    uint64_t fc_edges;
    int failures = 0;
    int face;
    int seq;

    /* Production INFO logging would emit thousands of "Session created" lines
     * and bury the structured probe output. Keep errors only. */
    logger_set_level(LOG_LEVEL_ERROR);

    snprintf(root, sizeof(root), "build/blocking_probe_io/run_%ld", (long)getpid());
    g_F_addr = (pthread_mutex_t *)file_mgr_mutex_addr_for_test();
    g_C_addr = &g_ctx.mutex;
    if (g_F_addr == NULL) {
        fprintf(stderr, "FATAL: file_mgr_mutex_addr_for_test() returned NULL\n");
        return 2;
    }
    if (dump_pcm_set_root_dir(root) != 0) {
        fprintf(stderr, "FATAL: dump_pcm_set_root_dir(%s) failed\n", root);
        return 2;
    }
    dump_pcm_set_enabled(1);
    if (dump_pcm_set_metadata_enabled(1) != 0) {
        fprintf(stderr, "FATAL: metadata sidecar could not be enabled\n");
        return 2;
    }

    /* Warm the lazy singleton so the first group is not measuring init. */
    dump_pcm_data_write_ex("abcd", 4, "warm", "up", &g_fmt);
    usleep(200000);
    dump_pcm_data_stop("warm", "up");
    usleep(100000);

    printf("[config] samples=%d warmup_skip=%d caller_interval=%dus session_interval=%dus "
           "queue_limit=production(default) root=%s\n",
           SAMPLES, WARMUP_SKIP, CALLER_INTERVAL_US, SESSION_INTERVAL_US, root);
    printf("[limits] baseline<=%dus injected<=%dus blocked>=10ms=0 dropped_delta<=%u fc_edges=0\n",
           LIMIT_BASELINE_US, LIMIT_INJECTED_US, (unsigned)LIMIT_DROPPED_DELTA);
    fflush(stdout);

    for (face = 0; face < 2; ++face) {
        for (seq = 0; seq < 4; ++seq) {
            if (run_group(face, seq, g_delays[seq], &groups[face][seq]) != 0) {
                fprintf(stderr, "FATAL: group face=%s seq=%d could not run\n", g_face_name[face], seq);
                return 2;
            }
            print_group("main", face, seq, &groups[face][seq]);
            fflush(stdout);
        }
    }

    /* Thresholds from §13.2. */
    for (face = 0; face < 2; ++face) {
        for (seq = 0; seq < 4; ++seq) {
            const group_result_t *r = &groups[face][seq];
            uint64_t limit = (r->delay_ms == 0) ? LIMIT_BASELINE_US : LIMIT_INJECTED_US;
            uint64_t wanted_hits = (face == 0) ? r->pcm_hits : r->sidecar_hits;
            uint64_t other_hits = (face == 0) ? r->sidecar_hits : r->pcm_hits;

            if (r->caller_max_us > limit) {
                fail(&failures, "face=%s seq=%d caller_max=%lluus exceeds %lluus",
                     g_face_name[face], seq, (unsigned long long)r->caller_max_us,
                     (unsigned long long)limit);
            }
            if (r->over_10ms != 0) {
                fail(&failures, "face=%s seq=%d blocked>=10ms count=%llu",
                     g_face_name[face], seq, (unsigned long long)r->over_10ms);
            }
            if (r->delay_ms > 0 && wanted_hits == 0) {
                fail(&failures, "face=%s seq=%d injection never hit its fopen face",
                     g_face_name[face], seq);
            }
            if (other_hits != 0) {
                fail(&failures, "face=%s seq=%d injection leaked to the other face (%llu hits)",
                     g_face_name[face], seq, (unsigned long long)other_hits);
            }
            if (seq > 0) {
                uint64_t a = groups[face][seq - 1].dropped;
                uint64_t b = r->dropped;
                uint64_t delta = (a > b) ? (a - b) : (b - a);

                if (delta > LIMIT_DROPPED_DELTA) {
                    fail(&failures, "face=%s seq=%d dropped_blocks delta=%llu exceeds %u",
                         g_face_name[face], seq, (unsigned long long)delta,
                         (unsigned)LIMIT_DROPPED_DELTA);
                }
            }
        }
    }

    /* Reverse control: sidecar-only 20 ms, no barrier at publish. The process
     * must exit normally, produce no F->C edge, and actually write sidecars. */
    sidecars_before = count_sidecars(root);
    if (run_group(1, 4, 20, &reverse) != 0) {
        fprintf(stderr, "FATAL: reverse control could not run\n");
        return 2;
    }
    sidecars_after = count_sidecars(root);
    print_group("reverse", 1, 4, &reverse);
    printf("[reverse] sidecar_json_before=%llu sidecar_json_after=%llu\n",
           (unsigned long long)sidecars_before, (unsigned long long)sidecars_after);
    if (sidecars_after <= sidecars_before) {
        fail(&failures, "reverse control produced no sidecar .json file");
    }
    if (reverse.sidecar_hits == 0) {
        fail(&failures, "reverse control never injected into sidecar fopen");
    }
    if (reverse.pcm_hits != 0) {
        fail(&failures, "reverse control leaked into the PCM face");
    }

    fc_edges = atomic_load(&g_fc_edges);
    printf("[audit] F->C edges=%llu (expected 0, ABBA=no)\n", (unsigned long long)fc_edges);
    if (fc_edges != 0) {
        fail(&failures, "F->C edges=%llu, ABBA cycle present", (unsigned long long)fc_edges);
    }

    printf("RESULT: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
