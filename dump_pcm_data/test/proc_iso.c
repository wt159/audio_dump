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
#include <windows.h>
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

/* True when `entry` ("NAME=value") carries the same NAME as `override`.
 * Windows also seeds the environment with "=C:=C:\path" entries, where the name
 * is empty; those must never match a real override. */
static int env_name_matches(const char *entry, const char *override) {
    const char *eq = strchr(override, '=');
    size_t name_len = (eq != NULL) ? (size_t)(eq - override) : strlen(override);

    if (name_len == 0 || strncmp(entry, override, name_len) != 0) {
        return 0;
    }
    return entry[name_len] == '=';
}

#ifdef _WIN32

/* Build a Windows environment block: NUL-separated "NAME=value" strings with a
 * second NUL at the end.
 *
 * Built from GetEnvironmentStringsA() and handed to CreateProcessA directly,
 * rather than going through the CRT's _spawnve with `_environ`. On MinGW the
 * spawn helper is linked into this binary as static runtime code, so it takes
 * part in the link-time --wrap interposition the suite uses for fault
 * injection; an access violation inside it is not something this layer can
 * debug or control. CreateProcessA is the actual contract anyway: create the
 * process, wait, read the exit code. */
static char *build_env_block(const char *const *overrides, size_t override_count) {
    char *source = GetEnvironmentStringsA();
    char *block = NULL;
    char *cursor = NULL;
    const char *scan = NULL;
    size_t total = 1;

    if (source == NULL) {
        return NULL;
    }

    scan = source;
    while (*scan != '\0') {
        size_t len = strlen(scan);
        int replaced = 0;

        for (size_t j = 0; j < override_count; ++j) {
            if (env_name_matches(scan, overrides[j])) {
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            total += len + 1;
        }
        scan += len + 1;
    }
    for (size_t j = 0; j < override_count; ++j) {
        total += strlen(overrides[j]) + 1;
    }

    block = (char *)malloc(total + 1);
    if (block == NULL) {
        FreeEnvironmentStringsA(source);
        return NULL;
    }

    cursor = block;
    scan = source;
    while (*scan != '\0') {
        size_t len = strlen(scan);
        int replaced = 0;

        for (size_t j = 0; j < override_count; ++j) {
            if (env_name_matches(scan, overrides[j])) {
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            memcpy(cursor, scan, len + 1);
            cursor += len + 1;
        }
        scan += len + 1;
    }
    for (size_t j = 0; j < override_count; ++j) {
        size_t len = strlen(overrides[j]);

        memcpy(cursor, overrides[j], len + 1);
        cursor += len + 1;
    }
    *cursor = '\0';

    FreeEnvironmentStringsA(source);
    return block;
}

#endif /* _WIN32 */

int iso_run(const char *const *env_overrides, iso_result_t *out) {
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

#ifdef _WIN32
    {
        char cmdline[1200];
        char *block = NULL;
        STARTUPINFOA startup;
        PROCESS_INFORMATION child;
        DWORD exit_code = 0;
        BOOL started = FALSE;

        block = build_env_block(env_overrides, override_count);
        if (block == NULL) {
            return -1;
        }

        /* Paths with spaces must be quoted; the command line is one mutable
         * buffer because CreateProcessA may write into it. */
        snprintf(cmdline, sizeof(cmdline), "\"%s\"", g_self_path);
        memset(&startup, 0, sizeof(startup));
        startup.cb = sizeof(startup);
        memset(&child, 0, sizeof(child));

        started = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, block, NULL, &startup,
                                 &child);
        free(block);
        if (!started) {
            return -1;
        }

        CloseHandle(child.hThread);
        WaitForSingleObject(child.hProcess, INFINITE);
        if (!GetExitCodeProcess(child.hProcess, &exit_code)) {
            CloseHandle(child.hProcess);
            return -1;
        }
        CloseHandle(child.hProcess);

        out->spawned = 1;
        out->exited = 1;
        out->exit_code = (int)exit_code;
        return 0;
    }
#else
    {
        char *argv[2];
        size_t base_count = 0;
        size_t base_out = 0;
        char **env = NULL;
        pid_t pid = 0;
        int status = 0;

        while (environ != NULL && environ[base_count] != NULL) {
            base_count++;
        }
        /* Replacement rather than append: getenv() returns the first match, so
         * an appended override would be invisible. */
        env = (char **)calloc(base_count + override_count + 1, sizeof(char *));
        if (env == NULL) {
            return -1;
        }
        for (size_t i = 0; i < base_count; ++i) {
            int replaced = 0;

            for (size_t j = 0; j < override_count; ++j) {
                if (env_name_matches(environ[i], env_overrides[j])) {
                    replaced = 1;
                    break;
                }
            }
            if (!replaced) {
                env[base_out++] = environ[i];
            }
        }
        for (size_t j = 0; j < override_count; ++j) {
            env[base_out++] = (char *)env_overrides[j];
        }
        env[base_out] = NULL;

        argv[0] = g_self_path;
        argv[1] = NULL;

        pid = fork();
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

        if (waitpid(pid, &status, 0) < 0) {
            return -1;
        }
        if (WIFEXITED(status)) {
            out->exited = 1;
            out->exit_code = WEXITSTATUS(status);
        }
        return 0;
    }
#endif
}
