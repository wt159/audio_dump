/* Process isolation for the PC test harness.
 *
 * Several cases need a pristine process to assert on module-wide state: lazy
 * singleton init, the low-storage latch, the one-shot retention scan, and the
 * init failure/retry paths. On POSIX the harness reached for
 * fork/exec/waitpid; Windows has no usable equivalent, so this header states
 * the contract the harness actually depends on rather than the mechanism:
 *
 *   spawn this same test binary with a few extra environment assignments,
 *   wait for it, and report how it exited.
 *
 * Deliberately narrow. There is no handle, no streaming output, no terminate,
 * and no timeout, because no case needs them: every caller is spawn-then-wait
 * and reads only the exit code. An unused surface is a liability.
 */
#ifndef DUMP_PCM_TEST_PROC_ISO_H
#define DUMP_PCM_TEST_PROC_ISO_H

#include <stddef.h>

typedef struct {
    int spawned;   /* 1 when the child process was created */
    int exited;    /* 1 when a normal exit status was collected */
    int exit_code; /* valid only when exited == 1 */
} iso_result_t;

/* Record how to re-execute this binary. Call once at the top of main(). */
void iso_init(const char *argv0);

/* Absolute path of the running test binary, or "" when unknown. */
const char *iso_self_path(void);

/* Spawn the test binary with extra KEY=VALUE overrides layered on the current
 * environment, then wait for it.
 *
 * Returns 0 when a child was spawned and waited for; -1 when it could not be.
 * A zero return does NOT mean the child succeeded: check iso_ok(), which
 * requires an explicit normal exit. Treating "no result" as success is how the
 * old fork()-based code silently passed a failing case. */
int iso_run(const char *const *env_overrides, iso_result_t *out);

static inline int iso_ok(const iso_result_t *r) {
    return r != NULL && r->spawned == 1 && r->exited == 1 && r->exit_code == 0;
}

#endif /* DUMP_PCM_TEST_PROC_ISO_H */
