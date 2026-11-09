#ifndef DUMP_PCM_SESSION_H
#define DUMP_PCM_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../include/dump_pcm_data.h"
#include "platform_config.h"

#define KEY1_MAX_LEN DUMP_PCM_KEY_MAX_LEN
#define KEY2_MAX_LEN DUMP_PCM_KEY_MAX_LEN

typedef enum {
    SESSION_ACTIVE = 0,
    SESSION_STOPPING = 1
} session_state_t;

/* What the writer should do with a session's file handle for the next block.
 * Pure decision so it can be tested without threads or timing. */
typedef enum {
    SESSION_FILE_USE = 0,
    SESSION_FILE_REOPEN = 1,
    SESSION_FILE_ROLLOVER = 2
} session_file_action_t;

typedef struct {
    char key1[KEY1_MAX_LEN + 1];
    char key2[KEY2_MAX_LEN + 1];
    FILE *fp;
    char file_path[256];
    uint64_t file_size;
    // Monotonic milliseconds recorded at public write call entry.
    uint64_t last_write_time;
    session_state_t state;
    bool accepting;
    uint32_t generation;
    uint32_t split_index;
    uint32_t pending_blocks;
    bool has_format;
    dump_pcm_format_t format;
    /* Wall-clock start of the current file, kept across handle eviction so a
     * reopened file still reports its original start time. */
    uint64_t file_start_unix_ms;
} session_info_t;

typedef struct {
    session_info_t *sessions[MAX_SESSIONS];
    uint32_t count;
} session_manager_t;

void session_mgr_init(session_manager_t *mgr);
void session_mgr_destroy(session_manager_t *mgr);
session_info_t *session_find(session_manager_t *mgr, const char *key1, const char *key2);
session_info_t *session_find_accepting(session_manager_t *mgr, const char *key1, const char *key2);
session_info_t *session_find_by_generation(session_manager_t *mgr, const char *key1, const char *key2,
                                           uint32_t generation);
session_info_t *session_create(session_manager_t *mgr, const char *key1, const char *key2, uint32_t generation);
void session_destroy(session_manager_t *mgr, session_info_t *session);
session_info_t *session_get_or_create(session_manager_t *mgr, const char *key1, const char *key2);
uint32_t session_count(session_manager_t *mgr);
uint32_t session_live_count(session_manager_t *mgr);

/* handle_tracked reports whether the file manager still owns session->fp.
 * A handle can be closed behind the session's back when MAX_OPEN_FILES is
 * reached, and writing through it would touch a closed FILE*. */
session_file_action_t session_file_action(const session_info_t *session, bool handle_tracked,
                                          uint64_t size_limit);

/* Format validation is pure; a session keeps the first valid format it sees. */
bool session_format_is_valid(const dump_pcm_format_t *format);
void session_capture_format(session_info_t *session, const dump_pcm_format_t *format);

#endif
