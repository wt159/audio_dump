/**
 * @file logger.h
 * @brief 日志抽象层接口
 */
#ifndef DUMP_PCM_LOGGER_H
#define DUMP_PCM_LOGGER_H

#include <stdarg.h>

/**
 * @brief 日志级别
 */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2
} log_level_t;

/**
 * @brief 日志输出函数类型
 * @param level 日志级别
 * @param tag   日志标签
 * @param fmt   格式化字符串
 * @param ...   可变参数
 */
typedef void (*log_fn_t)(log_level_t level, const char *tag, const char *fmt, ...);

/**
 * @brief 初始化日志模块
 * @param fn 自定义日志函数，传入 NULL 则使用默认实现（空操作）
 */
void logger_init(log_fn_t fn);

/**
 * @brief 获取当前日志级别
 */
log_level_t logger_get_level(void);

/**
 * @brief 设置日志级别
 */
void logger_set_level(log_level_t level);

/**
 * @brief 记录日志
 * @param level 日志级别
 * @param fmt   格式化字符串
 * @param ...   可变参数
 */
void logger_log(log_level_t level, const char *fmt, ...);

/**
 * @brief 便捷宏
 */
#define LOGE(fmt, ...) logger_log(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logger_log(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) logger_log(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)

#endif /* DUMP_PCM_LOGGER_H */
