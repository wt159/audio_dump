#include "session.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "key_utils.h"
#include "logger.h"

static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t hash_key(const char *key1, const char *key2) {
    return dump_key_hash_pair(key1, key2) % MAX_SESSIONS;
}

static session_info_t *session_find_highest(session_manager_t *mgr, const char *key1, const char *key2,
                                            bool accepting_only) {
    session_info_t *best = NULL;
    uint32_t idx = 0;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2)) {
        return NULL;
    }

    idx = hash_key(key1, key2);
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        uint32_t pos = (idx + i) % MAX_SESSIONS;
        session_info_t *session = mgr->sessions[pos];
        if (session == NULL || strcmp(session->key1, key1) != 0 || strcmp(session->key2, key2) != 0) {
            continue;
        }
        if (accepting_only && (!session->accepting || session->state != SESSION_ACTIVE)) {
            continue;
        }
        if (best == NULL || session->generation > best->generation) {
            best = session;
        }
    }

    return best;
}

void session_mgr_init(session_manager_t *mgr) {
    if (mgr == NULL) {
        return;
    }

    memset(mgr->sessions, 0, sizeof(mgr->sessions));
    mgr->count = 0;
}

void session_mgr_destroy(session_manager_t *mgr) {
    if (mgr == NULL) {
        return;
    }

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        free(mgr->sessions[i]);
        mgr->sessions[i] = NULL;
    }
    mgr->count = 0;
}

session_info_t *session_find(session_manager_t *mgr, const char *key1, const char *key2) {
    return session_find_highest(mgr, key1, key2, false);
}

session_info_t *session_find_accepting(session_manager_t *mgr, const char *key1, const char *key2) {
    return session_find_highest(mgr, key1, key2, true);
}

session_info_t *session_find_by_generation(session_manager_t *mgr, const char *key1, const char *key2,
                                           uint32_t generation) {
    uint32_t idx = 0;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2)) {
        return NULL;
    }

    idx = hash_key(key1, key2);
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        uint32_t pos = (idx + i) % MAX_SESSIONS;
        session_info_t *session = mgr->sessions[pos];
        if (session != NULL && strcmp(session->key1, key1) == 0 && strcmp(session->key2, key2) == 0 &&
            session->generation == generation) {
            return session;
        }
    }

    return NULL;
}

session_info_t *session_create(session_manager_t *mgr, const char *key1, const char *key2, uint32_t generation) {
    uint32_t idx = 0;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2)) {
        return NULL;
    }

    if (mgr->count >= MAX_SESSIONS) {
        LOGW("Max sessions reached: %u", MAX_SESSIONS);
        return NULL;
    }

    idx = hash_key(key1, key2);
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        uint32_t pos = (idx + i) % MAX_SESSIONS;
        if (mgr->sessions[pos] == NULL) {
            session_info_t *session = calloc(1, sizeof(*session));
            if (session == NULL) {
                LOGE("Failed to allocate session");
                return NULL;
            }

            dump_key_copy(session->key1, key1);
            dump_key_copy(session->key2, key2);
            session->last_write_time = get_timestamp_ms();
            session->state = SESSION_ACTIVE;
            session->accepting = true;
            session->generation = generation;
            session->split_index = 0;
            mgr->sessions[pos] = session;
            mgr->count++;
            LOGI("Session created: key1=%s key2=%s generation=%u", session->key1, session->key2,
                 generation);
            return session;
        }
    }

    return NULL;
}

void session_destroy(session_manager_t *mgr, session_info_t *session) {
    if (mgr == NULL || session == NULL) {
        return;
    }

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        if (mgr->sessions[i] == session) {
            free(session);
            mgr->sessions[i] = NULL;
            mgr->count--;
            LOGI("Session destroyed");
            return;
        }
    }
}

session_info_t *session_get_or_create(session_manager_t *mgr, const char *key1, const char *key2) {
    session_info_t *session = NULL;

    if (mgr == NULL || !dump_key_is_valid(key1) || !dump_key_is_valid(key2)) {
        return NULL;
    }

    session = session_find_accepting(mgr, key1, key2);
    if (session != NULL) {
        return session;
    }

    return session_create(mgr, key1, key2, 1);
}

uint32_t session_count(session_manager_t *mgr) {
    return (mgr == NULL) ? 0 : mgr->count;
}

uint32_t session_live_count(session_manager_t *mgr) {
    uint32_t count = 0;

    if (mgr == NULL) {
        return 0;
    }

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        session_info_t *session = mgr->sessions[i];
        if (session != NULL && (session->state == SESSION_ACTIVE || session->state == SESSION_STOPPING)) {
            count++;
        }
    }

    return count;
}

session_file_action_t session_file_action(const session_info_t *session, bool handle_tracked,
                                          uint64_t size_limit) {
    if (session == NULL || session->fp == NULL) {
        return SESSION_FILE_REOPEN;
    }
    if (!handle_tracked) {
        return SESSION_FILE_REOPEN;
    }
    if (size_limit > 0 && session->file_size >= size_limit) {
        return SESSION_FILE_ROLLOVER;
    }

    return SESSION_FILE_USE;
}
