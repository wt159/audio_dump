#ifndef DUMP_PCM_KEY_UTILS_H
#define DUMP_PCM_KEY_UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/dump_pcm_data.h"

static inline int dump_key_is_valid(const char *key) {
    return key != NULL && key[0] != '\0' && strlen(key) <= DUMP_PCM_KEY_MAX_LEN;
}

static inline void dump_key_copy(char dst[DUMP_PCM_KEY_MAX_LEN + 1], const char *src) {
    if (dst == NULL) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, DUMP_PCM_KEY_MAX_LEN);
    dst[DUMP_PCM_KEY_MAX_LEN] = '\0';
}

static inline void dump_key_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0' && j < dst_size - 1; ++i) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            dst[j++] = c;
        } else {
            dst[j++] = '_';
        }
    }

    dst[j] = '\0';
}

static inline uint32_t dump_key_hash_pair(const char *key1, const char *key2) {
    const unsigned char *p = NULL;
    uint32_t hash = 2166136261u;

    for (p = (const unsigned char *)key1; p != NULL && *p != '\0'; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }

    hash ^= 0xffu;
    hash *= 16777619u;

    for (p = (const unsigned char *)key2; p != NULL && *p != '\0'; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }

    return hash;
}

#endif
