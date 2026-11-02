#include "logger.h"
#include "platform_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef DUMP_PCM_PC
static void pc_log_fn(log_level_t level, const char *tag, const char *fmt, ...) {
    const char *level_str[] = {"ERROR", "WARN", "INFO"};
    printf("[%s][%s] ", level_str[level], tag);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
#else
#include <android/log.h>
static void android_log_fn(log_level_t level, const char *tag, const char *fmt, ...) {
    int android_level = (level == LOG_LEVEL_ERROR) ? ANDROID_LOG_ERROR :
                        (level == LOG_LEVEL_WARN) ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    __android_log_print(android_level, tag, "%s", msg);
}
#endif

static log_fn_t g_log_fn = NULL;
static log_level_t g_log_level = LOG_LEVEL_INFO;
static const char *g_log_tag = LOG_TAG;

void logger_init(log_fn_t fn) {
#ifdef DUMP_PCM_PC
    if (fn == NULL) {
        g_log_fn = pc_log_fn;
    } else {
        g_log_fn = fn;
    }
#else
    if (fn == NULL) {
        g_log_fn = android_log_fn;
    } else {
        g_log_fn = fn;
    }
#endif
}

log_level_t logger_get_level(void) {
    return g_log_level;
}

void logger_set_level(log_level_t level) {
    g_log_level = level;
}

void logger_log(log_level_t level, const char *fmt, ...) {
    if (level > g_log_level) {
        return;
    }

    if (g_log_fn != NULL) {
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        g_log_fn(level, g_log_tag, "%s", msg);
    }
}