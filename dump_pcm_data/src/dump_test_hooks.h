#ifndef DUMP_PCM_TEST_HOOKS_H
#define DUMP_PCM_TEST_HOOKS_H

#include <pthread.h>
#include <stdint.h>

/* Test-only injection points. This header is not installed and is not part of
 * the public API; the public header stays free of <sys/statvfs.h> and
 * <pthread.h> so that desktop consumers can include it anywhere. */

typedef int (*dump_storage_query_fn_t)(const char *path, uint64_t *available_bytes);
typedef int (*dump_pthread_create_fn_t)(pthread_t *thread, const pthread_attr_t *attr,
                                        void *(*start_routine)(void *), void *arg);
typedef uint64_t (*dump_time_ms_fn_t)(void);

#if defined(DUMP_PCM_PC) || !defined(ANDROID)
void dump_pcm_set_storage_query_fn_for_test(dump_storage_query_fn_t fn);
void dump_pcm_set_pthread_create_fn_for_test(dump_pthread_create_fn_t fn);
void dump_pcm_set_time_ms_fn_for_test(dump_time_ms_fn_t fn);
void dump_pcm_set_file_size_limit_for_test(uint64_t bytes);
#endif

#endif /* DUMP_PCM_TEST_HOOKS_H */
