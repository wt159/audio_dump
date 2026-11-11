/* Process isolation: one implementation, two branches.
 *
 * Keeping both branches in one translation unit avoids a platform-selected
 * source list in the Makefile, CMake target and Windows script. The two halves
 * never compile together, so the only cost is a guard around each include.
 */
#include "proc_iso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_self_path[1024];

#ifdef _WIN32
#include <process.h>
#include <windows.h>
extern char **_environ;
#else
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

void iso_init(const char *argv0) {
#ifdef _WIN32
    DWORD len = 0;

    (void)argv0;
    /* The child must be re-executable from any working directory, so take the
     * module path rather than argv[0]. */
    len = GetModuleFileNameA(NULL, g_self_path, (DWORD)sizeof(g_self_path));
    if (len == 0 || len >= (DWORD)sizeof(g_self_path)) {
        g_self_path[0] = '\0';
    }
#else
    if (argv0 != NULL && argv0[0] != '\0') {
        strncpy(g_self_path, argv0, sizeof(g_self_path) - 1);
        g_self_path[sizeof(g_self_path) - 1] = '\0';
    }
#endif
}

const char *iso_self_path(void) {
    return g_self_path;
}

static char **base_environ(void) {
#ifdef _WIN32
    return _environ;
#else
    return environ;
#endif
}

/* True when `entry` ("NAME=value") carries the same NAME as `override`. */
static int env_name_matches(const char *entry, const char *override) {
    const char *eq = strchr(override, '=');
    size_t name_len = (eq != NULL) ? (size_t)(eq - override) : strlen(override);

    if (strncmp(entry, override, name_len) != 0) {
        return 0;
    }
    return entry[name_len] == '=';
}

/* Duplicate the environment with overrides replacing same-named entries.
 *
 * Replacement rather than append matters: POSIX leaves duplicates
 * unspecified and glibc's getenv() returns the first match, so an appended
 * override would be invisible. Strings stay borrowed from the caller and from
 * the process environment.
 */
static char **build_env(const char *const *overrides, size_t override_count) {
    char **base = base_environ();
    size_t base_count = 0;
    size_t out = 0;
    char **env = NULL;

    while (base != NULL && base[base_count] != NULL) {
        base_count++;
    }

    env = (char **)calloc(base_count + override_count + 1, sizeof(char *));
    if (env == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < base_count; ++i) {
        int replaced = 0;

        for (size_t j = 0; j < override_count; ++j) {
            if (env_name_matches(base[i], overrides[j])) {
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            env[out++] = base[i];
        }
    }
    for (size_t j = 0; j < override_count; ++j) {
        env[out++] = (char *)overrides[j];
    }
    env[out] = NULL;
    return env;
}

int iso_run(const char *const *env_overrides, iso_result_t *out) {
    char *argv[2];
    char **env = NULL;
    size_t override_count = 0;

    if (out == NULL) {
        return -1;
    }
    out->spawned = 0;
    out->exited = 0;
    out->exit_code = -1;

    if (g_self_path[0] == '\0') {
        return -1;
    }

    while (env_overrides != NULL && env_overrides[override_count] != NULL) {
        override_count++;
    }

    env = build_env(env_overrides, override_count);
    if (env == NULL) {
        return -1;
    }

    argv[0] = g_self_path;
    argv[1] = NULL;

#ifdef _WIN32
    {
        /* _P_WAIT blocks and yields the exit status directly, so there is no
         * separate wait step here. */
        intptr_t status = _spawnve(_P_WAIT, g_self_path, (const char *const *)argv,
                                   (const char *const *)env);

        free(env);
        if (status == -1) {
            return -1;
        }
        out->spawned = 1;
        out->exited = 1;
        out->exit_code = (int)status;
        return 0;
    }
#else
    {
        pid_t pid = fork();

        if (pid < 0) {
            free(env);
            return -1;
        }
        if (pid == 0) {
            execve(g_self_path, argv, env);
            _exit(127);
        }
        free(env);
        out->spawned = 1;

        {
            int status = 0;

            if (waitpid(pid, &status, 0) < 0) {
                return -1;
            }
            if (WIFEXITED(status)) {
                out->exited = 1;
                out->exit_code = WEXITSTATUS(status);
            }
        }
        return 0;
    }
#endif
}
