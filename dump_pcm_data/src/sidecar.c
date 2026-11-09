#include "sidecar.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "session.h"

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
