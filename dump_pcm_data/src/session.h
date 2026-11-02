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

#endif
