/* Platform shims for the PC test harness.
 *
 * Everything the harness needs that MinGW-w64 does not spell the POSIX way.
 * Deliberately header-only and inline: the alternatives are an extra object
 * file in three build systems, or a pile of #ifdefs at 60 call sites.
 *
 * This is test-only by construction. Production code keeps its own
 * DUMP_MKDIR in src/platform_config*.h; the harness must not depend on a
 * private module header to build.
 */
#ifndef DUMP_PCM_TEST_PLAT_H
#define DUMP_PCM_TEST_PLAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* mkdir with the POSIX argument shape. MinGW only offers _mkdir(path). */
static inline int tp_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/* setenv(name, value, 1). MinGW has no setenv at all; _putenv_s is the
 * replacement and also honours the "empty value removes the variable" rule
 * that unsetenv needs. */
static inline int tp_setenv(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static inline int tp_unsetenv(const char *name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

/* Root directory for the suite's scratch files.
 *
 * AUDIO_DUMP_DIR wins because the runner script uses it to keep the whole
 * suite inside a disposable directory. The fallbacks differ per platform: on
 * Windows a bare relative path keeps the tree off C:\ entirely.
 */
static inline const char *tp_test_root(void) {
    const char *dir = getenv("AUDIO_DUMP_DIR");

    if (dir != NULL && dir[0] != '\0') {
        return dir;
    }
#ifdef _WIN32
    return "audio_dump_test";
#else
    return "/tmp/audio_dump_test";
#endif
}

#endif /* DUMP_PCM_TEST_PLAT_H */
