/* Windows runtime smoke test for the PCM dump module.
 *
 * The Linux harness cannot run here: it depends on fork/exec/waitpid. This
 * program exercises what only Windows can verify at runtime - _mkdir directory
 * creation, GetDiskFreeSpaceExA probing, backslash normalisation, the writer
 * thread, and the MoveFileExA sidecar rename.
 *
 * Usage: windows_smoke.exe <output-dir>   (directory is created if missing)
 * Exit code 0 means every check passed.
 */
#define _POSIX_C_SOURCE 200809L
#define DUMP_PCM_INTERNAL_DEBUG_API 1

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "dump_pcm_data.h"

#define PCM_BYTES 8192

static int failures = 0;
static const char *g_root = NULL;

static void check(int condition, const char *what) {
    printf("%s: %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        failures++;
    }
}

static int count_files(const char *pattern) {
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    int count = 0;

    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        count++;
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    return count;
}

static int read_first_file(const char *pattern, char *buf, size_t size) {
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD got = 0;
    char path[MAX_PATH];
    int result = -1;

    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    FindClose(handle);

    snprintf(path, sizeof(path), "%s%s", g_root, data.cFileName);
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (ReadFile(file, buf, (DWORD)size - 1, &got, NULL)) {
        buf[got] = '\0';
        result = (int)got;
    }
    CloseHandle(file);
    return result;
}

int main(int argc, char **argv) {
    dump_pcm_format_t format = {48000, 2, 16, DUMP_PCM_FORMAT_S16_LE};
    dump_status_t status = {0};
    char pcm[PCM_BYTES];
    char json[2048];
    char pattern[MAX_PATH];
    char late[MAX_PATH];

    if (argc < 2) {
        printf("usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    g_root = argv[1];

    memset(pcm, 0x5a, sizeof(pcm));

    /* Nothing enabled yet: the query API must refuse. */
    check(dump_pcm_data_get_status(&status) == -1, "status before init returns -1");

    /* Backslashes on purpose: the module must normalise them. */
    check(dump_pcm_set_root_dir(g_root) == 0, "set_root_dir accepts a Windows path");
    check(dump_pcm_set_metadata_enabled(1) == 0, "metadata switch accepts enable");
    dump_pcm_set_enabled(1);

    dump_pcm_data_write_ex(pcm, sizeof(pcm), "smoke", "out", &format);
    Sleep(1200);
    dump_pcm_data_stop("smoke", "out");
    Sleep(400);

    check(dump_pcm_data_get_status(&status) == 0, "status after write succeeds");
    check(status.enabled, "module reports enabled");
    check(status.available_storage_mb >= 500,
          "GetDiskFreeSpaceExA reports at least the 500MB threshold");
    printf("     available_storage_mb=%llu\n", (unsigned long long)status.available_storage_mb);

    snprintf(pattern, sizeof(pattern), "%s*.pcm", g_root);
    check(count_files(pattern) == 1, "exactly one .pcm file");

    snprintf(pattern, sizeof(pattern), "%s*.json", g_root);
    check(count_files(pattern) == 1, "exactly one .json sidecar");

    if (read_first_file(pattern, json, sizeof(json)) > 0) {
        check(strstr(json, "\"complete\": true") != NULL, "sidecar marked complete");
        check(strstr(json, "\"total_bytes\": 8192") != NULL, "sidecar total_bytes matches");
        check(strstr(json, "\"sample_rate\": 48000") != NULL, "sidecar carries the format");
        check(strstr(json, "\"pcm_file\":") != NULL, "sidecar names the pcm file");
    } else {
        check(0, "sidecar readable");
    }

    snprintf(late, sizeof(late), "%slate\\", g_root);
    check(dump_pcm_set_root_dir(late) == -1, "set_root_dir refuses after init");

    printf("\n%s (%d failure%s)\n", failures == 0 ? "SMOKE PASSED" : "SMOKE FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
