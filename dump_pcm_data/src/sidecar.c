#include "sidecar.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "session.h"

#define SIDECAR_PATH_MAX 1024
#define SIDECAR_JSON_MAX 4096

static atomic_int g_sidecar_enabled = 0;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} json_writer_t;

static void jw_put(json_writer_t *w, const char *s, size_t n) {
    if (!w->ok) {
        return;
    }
    if (w->len + n + 1 > w->cap) {
        w->ok = false;
        return;
    }

    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_raw(json_writer_t *w, const char *s) {
    jw_put(w, s, strlen(s));
}

static void jw_fmt(json_writer_t *w, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    int n = 0;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        w->ok = false;
        return;
    }
    jw_put(w, tmp, (size_t)n);
}

static void jw_string(json_writer_t *w, const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    jw_put(w, "\"", 1);
    for (; *p != '\0'; ++p) {
        switch (*p) {
        case '"':
            jw_raw(w, "\\\"");
            break;
        case '\\':
            jw_raw(w, "\\\\");
            break;
        case '\b':
            jw_raw(w, "\\b");
            break;
        case '\f':
            jw_raw(w, "\\f");
            break;
        case '\n':
            jw_raw(w, "\\n");
            break;
        case '\r':
            jw_raw(w, "\\r");
            break;
        case '\t':
            jw_raw(w, "\\t");
            break;
        default:
            if (*p < 0x20) {
                jw_fmt(w, "\\u%04x", (unsigned int)*p);
            } else {
                jw_put(w, (const char *)p, 1);
            }
            break;
        }
    }
    jw_put(w, "\"", 1);
}

bool sidecar_is_valid_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    if (s == NULL) {
        return false;
    }

    while (*p != '\0') {
        unsigned int cp = 0;
        int extra = 0;

        if (*p < 0x80) {
            ++p;
            continue;
        }
        if ((*p & 0xE0) == 0xC0) {
            extra = 1;
            cp = (unsigned int)(*p & 0x1F);
        } else if ((*p & 0xF0) == 0xE0) {
            extra = 2;
            cp = (unsigned int)(*p & 0x0F);
        } else if ((*p & 0xF8) == 0xF0) {
            extra = 3;
            cp = (unsigned int)(*p & 0x07);
        } else {
            return false;
        }
        ++p;

        for (int i = 0; i < extra; ++i, ++p) {
            if ((*p & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (unsigned int)(*p & 0x3F);
        }

        if (extra == 1 && cp < 0x80) {
            return false;
        }
        if (extra == 2 && cp < 0x800) {
            return false;
        }
        if (extra == 3 && cp < 0x10000) {
            return false;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
    }

    return true;
}

const char *sidecar_format_name(dump_pcm_sample_format_t format) {
    switch (format) {
    case DUMP_PCM_FORMAT_S16_LE:
        return "s16_le";
    case DUMP_PCM_FORMAT_S24_LE:
        return "s24_le";
    case DUMP_PCM_FORMAT_S24_3LE:
        return "s24_3le";
    case DUMP_PCM_FORMAT_S32_LE:
        return "s32_le";
    case DUMP_PCM_FORMAT_FLOAT_LE:
        return "float_le";
    default:
        return NULL;
    }
}

int sidecar_build_json(char *buf, size_t size, const char *pcm_file_name,
                       const dump_pcm_file_meta_t *meta, bool complete,
                       uint64_t total_bytes, uint64_t end_time_unix_ms) {
    json_writer_t w;
    const char *format_name = NULL;

    if (buf == NULL || size == 0 || pcm_file_name == NULL || meta == NULL) {
        return -1;
    }
    if (!session_format_is_valid(&meta->format)) {
        return -1;
    }
    if (!sidecar_is_valid_utf8(pcm_file_name) || !sidecar_is_valid_utf8(meta->key1) ||
        !sidecar_is_valid_utf8(meta->key2)) {
        return -1;
    }
    format_name = sidecar_format_name(meta->format.sample_format);
    if (format_name == NULL) {
        return -1;
    }

    w.buf = buf;
    w.cap = size;
    w.len = 0;
    w.ok = true;
    buf[0] = '\0';

    jw_raw(&w, "{\n  \"version\": 1,\n  \"pcm_file\": ");
    jw_string(&w, pcm_file_name);
    jw_raw(&w, ",\n  \"key1\": ");
    jw_string(&w, meta->key1);
    jw_raw(&w, ",\n  \"key2\": ");
    jw_string(&w, meta->key2);
    jw_fmt(&w, ",\n  \"generation\": %u", meta->generation);
    jw_fmt(&w, ",\n  \"split_index\": %u", meta->split_index);
    jw_fmt(&w, ",\n  \"sample_rate\": %u", meta->format.sample_rate);
    jw_fmt(&w, ",\n  \"channel_count\": %u", (unsigned int)meta->format.channel_count);
    jw_fmt(&w, ",\n  \"bits_per_sample\": %u", (unsigned int)meta->format.bits_per_sample);
    jw_fmt(&w, ",\n  \"sample_format\": \"%s\"", format_name);
    jw_fmt(&w, ",\n  \"start_time_unix_ms\": %llu", (unsigned long long)meta->start_time_unix_ms);

    if (complete) {
        jw_fmt(&w, ",\n  \"end_time_unix_ms\": %llu", (unsigned long long)end_time_unix_ms);
        jw_fmt(&w, ",\n  \"total_bytes\": %llu", (unsigned long long)total_bytes);
        jw_raw(&w, ",\n  \"complete\": true\n}\n");
    } else {
        jw_raw(&w, ",\n  \"complete\": false\n}\n");
    }

    return w.ok ? (int)w.len : -1;
}

static const char *sidecar_basename(const char *path) {
    const char *base = path;

    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }

    return base;
}

/* Derives <stem>.json and <stem>.json.tmp from pcm_path. Both platforms accept
 * '/' here; the Windows API tolerates forward slashes as well. */
static bool sidecar_build_paths(const char *pcm_path, char *json_path, size_t json_size,
                                char *tmp_path, size_t tmp_size) {
    size_t len = 0;
    size_t stem = 0;
    const char *base = NULL;

    if (pcm_path == NULL) {
        return false;
    }

    len = strlen(pcm_path);
    if (len == 0 || len + sizeof(".json") > json_size || len + sizeof(".json") + sizeof(".tmp") - 1 > tmp_size) {
        return false;
    }

    memcpy(json_path, pcm_path, len + 1);
    base = sidecar_basename(pcm_path);
    stem = (size_t)(base - pcm_path);
    for (size_t i = len; i > stem; --i) {
        if (json_path[i - 1] == '.') {
            len = i - 1;
            break;
        }
    }

    memcpy(json_path + len, ".json", sizeof(".json"));
    memcpy(tmp_path, json_path, len + sizeof(".json"));
    memcpy(tmp_path + len + sizeof(".json") - 1, ".tmp", sizeof(".tmp"));
    return true;
}

bool sidecar_set_enabled(int enabled) {
    atomic_store(&g_sidecar_enabled, enabled != 0 ? 1 : 0);
    return atomic_load(&g_sidecar_enabled) != 0;
}

bool sidecar_is_enabled(void) {
    return atomic_load(&g_sidecar_enabled) != 0;
}

uint64_t sidecar_now_unix_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int sidecar_write_file(const char *pcm_path, const char *json, size_t json_len) {
    char json_path[SIDECAR_PATH_MAX];
    char tmp_path[SIDECAR_PATH_MAX];
    FILE *fp = NULL;

    if (json == NULL || !sidecar_build_paths(pcm_path, json_path, sizeof(json_path), tmp_path, sizeof(tmp_path))) {
        return -1;
    }

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return -1;
    }
    if ((json_len > 0 && fwrite(json, 1, json_len, fp) != json_len) || fflush(fp) != 0 || ferror(fp)) {
        fclose(fp);
        remove(tmp_path);
        return -1;
    }
    if (fclose(fp) != 0) {
        remove(tmp_path);
        return -1;
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path, json_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp_path);
        return -1;
    }
#else
    if (rename(tmp_path, json_path) != 0) {
        remove(tmp_path);
        return -1;
    }
#endif

    return 0;
}

int sidecar_write_meta(const char *pcm_path, const dump_pcm_file_meta_t *meta, bool complete,
                       uint64_t total_bytes, uint64_t end_time_unix_ms) {
    char buf[SIDECAR_JSON_MAX];
    int len = 0;

    if (pcm_path == NULL || meta == NULL) {
        return -1;
    }

    len = sidecar_build_json(buf, sizeof(buf), sidecar_basename(pcm_path), meta, complete, total_bytes,
                             end_time_unix_ms);
    if (len <= 0) {
        return -1;
    }

    return sidecar_write_file(pcm_path, buf, (size_t)len);
}
