# Android Audio HAL PCM Dump 模块技术方案

## 1. 概述

本技术方案描述 Android Audio HAL PCM Dump 模块的实现细节，用于指导后续编码工作。

### 1.1 模块定位

- 模块用于抓取 PCM 原始音频数据并落盘，供调试、问题复现和离线分析使用
- 语言：C11 标准
- 调用方：Audio HAL 的 `out_write()` / `in_read()` / `out_standby()` / `in_standby()`

---

## 2. 架构设计

### 2.1 整体架构

```
+----------------+     +------------------+     +----------------+
|   HAL 线程      | --> |   生产者队列     | --> |  后台写线程    |
| (write/stop)   |     |  (数据块拷贝)    |     |  (文件落盘)    |
+----------------+     +------------------+     +----------------+
                              |                        |
                              v                        v
                    +------------------+     +----------------+
                    |   会话管理        |     |   文件管理      |
                    | (字符串 key1+key2)|     | (句柄/分片/清理)|
                    +------------------+     +----------------+
```

### 2.2 模块结构

```
dump_pcm_data/
├── include/
│   └── dump_pcm_data.h   # 公共接口头文件（含内部调试 API 宏门控）
├── src/
│   ├── dump_pcm_data.c   # API facade + init/shutdown 编排
│   ├── dump_control.c    # enable/storage/query 控制逻辑
│   ├── dump_writer.c     # 后台写线程：决策 / 执行 / 发布三段式
│   ├── file_mgr.c        # 文件槽位表、capture/run job、清理
│   ├── file_mgr_job.h    # POD job 与会话快照（仅内部）
│   ├── sidecar.c         # 元数据 JSON 序列化与原子写
│   ├── session.c         # 会话管理
│   ├── queue.c           # 队列实现
│   ├── logger.c          # 日志实现
│   ├── dump_internal.h   # 共享内部状态和不变量
│   ├── dump_test_hooks.h # 仅测试使用的注入点（不安装）
│   ├── logger.h / queue.h / session.h / file_mgr.h / sidecar.h  # 内部头
│   ├── key_utils.h       # 键值工具
│   └── platform_config.h / platform_config_default.h / platform_config_pc.h
└── test/ + tools/        # PC 测试、契约脚本与 §13.2 阻塞放大探针；
                          # 完整清单见 dump_pcm_data/README.md 的「项目结构」
```

源码树只在这里列到「模块职责」粒度：逐文件清单与新增文件由
[`dump_pcm_data/README.md`](../dump_pcm_data/README.md) 维护，避免两处各写一份、其中一份先腐烂。

---

## 3. 核心数据结构

### 3.1 数据块（待写入的 PCM 数据）

```c
typedef struct {
    char key1[KEY1_MAX_LEN + 1];
    char key2[KEY2_MAX_LEN + 1];
    char *buffer;              // 拷贝后的数据
    uint32_t buffer_size;
    uint64_t write_call_entry_time; // dump_pcm_data_write() 调用进入时间，由 HAL 线程在接口入口捕获
} data_block_t;
```

### 3.2 会话信息

```c
typedef struct {
    char key1[KEY1_MAX_LEN + 1];
    char key2[KEY2_MAX_LEN + 1];
    uint32_t generation;          // 同 key 新会话递增
    uint32_t split_index;         // 同会话内分片序号
    FILE *fp;                     // 当前文件句柄
    uint64_t file_size;           // 当前文件大小
    uint64_t last_write_call_entry_time; // 上一次 write 进入时间
    uint64_t last_handle_use_time;       // 句柄 LRU 用
    uint8_t active;               // 是否仍接收该会话的新数据
} session_info_t;
```

### 3.3 模块全局状态

```c
typedef struct {
    pthread_mutex_t mutex;      /* 主锁 C：队列 / 会话表 / pending_blocks 记账 */
    pthread_cond_t cond;        /* 写线程等待新 block */
    clockid_t cond_clock_id;    /* cond 创建时用的时钟；timedwait 必须用它算 deadline */
    pthread_t writer_thread;
    queue_t queue;              /* 值类型，不是指针 */
    session_manager_t sessions;
    file_manager_t file_mgr;    /* 持有自己的锁 F，与 C 分开 */
    int initialized;
    int writer_running;
    atomic_int low_storage_triggered;
    atomic_uint_fast64_t last_storage_check;
    atomic_uint_fast64_t storage_cache_mb;
    atomic_int enabled;
    uint32_t next_generation;
} dump_context_t;
```

### 3.4 运行时状态结构（调试接口用）

```c
typedef struct {
    uint8_t enabled;              // 总开关状态
    uint8_t low_storage_triggered;
    uint64_t available_storage_mb;
    uint32_t queue_pending_blocks;
    uint64_t queue_pending_bytes;
    uint32_t active_session_count;
} dump_status_t;

typedef struct {
    char key1[KEY1_MAX_LEN + 1];
    char key2[KEY2_MAX_LEN + 1];
    uint64_t written_bytes;
} dump_session_info_t;
```

---

## 4. 接口定义

### 4.1 公开接口

```c
// 写入 PCM 数据（线程安全，可被多个 HAL 线程并发调用）
void dump_pcm_data_write(const char *buffer, const uint32_t buffer_size,
                         const char *key1, const char *key2);

// 停止指定会话
void dump_pcm_data_stop(const char *key1, const char *key2);
```

以下内部调试接口在 `dump_pcm_data.h` 中通过 `DUMP_PCM_INTERNAL_DEBUG_API` 宏门控，不在共享库导出符号中：

```c
#ifdef DUMP_PCM_INTERNAL_DEBUG_API
// 获取模块状态
int dump_pcm_data_get_status(dump_status_t *status);

// 获取活跃会话列表
int dump_pcm_data_get_active_sessions(dump_session_info_t *sessions,
                                      int max_count, int *actual_count);
#endif
```

### 4.2 内部初始化接口

```c
// 内部使用，首次 write 时自动调用
static int dump_pcm_data_init(void);
```

---

## 5. 核心模块设计

### 5.1 队列模块（queue.c）

**设计思路**：使用环形队列或链表实现生产者-消费者模型

```c
typedef struct queue_node {
    data_block_t block;
    struct queue_node *next;
} queue_node_t;

typedef struct {
    queue_node_t *head;
    queue_node_t *tail;
    uint32_t count;           // 当前块数
    uint64_t total_bytes;     // 总字节数
    uint32_t max_blocks;     // 最多800块
    uint64_t max_bytes;      // 最多512MB
} queue_t;
```

**关键操作**：

- `queue_push()` - HAL 线程调用，需加锁
- `queue_pop()` - 后台写线程调用，需加锁
- `queue_is_full()` - 判断是否超限
- `queue_clear()` - 清空队列（stop 时使用）

### 5.2 会话管理模块（session.c）

**设计思路**：按 `key1 + key2` 管理当前活跃会话，记录 write 的 call-entry 时间，用于 2 秒会话滚转判断。

- generation 只在新 session 时递增，split-index 只在同一 session 内分片时递增

```c
// 会话查找：使用 key1 + key2 组合作为 key
// 使用线性探测哈希表实现
#define SESSION_TABLE_SIZE 32

typedef struct {
    session_info_t *table[SESSION_TABLE_SIZE];
} session_manager_t;
```

**关键操作**：

- `session_find()` - 查找会话，不存在则返回 NULL
- `session_create()` - 创建新会话，generation +1，split-index 归零
- `session_destroy()` - 销毁会话
- `session_find_accepting()` - 查找当前仍接收新数据的会话
- `session_should_rollover()` - 比较相邻 `dump_pcm_data_write()` 进入时间，超过 2 秒则触发隐式 stop 语义

### 5.3 文件管理模块（file_mgr.c）

**设计思路**：

- 根目录：Android 默认 `/data/vendor/audio/`，PC 测试默认 `/tmp/audio_dump_test/`；宿主可在初始化前用 `dump_pcm_set_root_dir()` 改
- 文件路径：`<root>/<escaped-key1>_<escaped-key2>_<stable-hash>_<generation>_<split-index>_<timestamp>.pcm`
- key2 转义：非 `[a-zA-Z0-9_-]` 字符替换为 `_`
- 分片策略：单文件 1GB 超过后，只在同一 session 内新建文件，generation 不变，split-index 加 1
- LRU 策略：最多 16 个打开的句柄，句柄空闲 5 秒后关闭最久未使用的文件；这只管句柄，不触发 session rollover

**关键操作**（生产路径走 capture / run 两族，见 [`stage2-lock-order-spec.md`](stage2-lock-order-spec.md) §5）：

- `file_mgr_capture_open()` / `_write()` / `_close()` / `_initial_meta()` / `_cleanup()` - 持 F 的纯内存阶段，只产出自包含 POD job，返回前必定释放 F
- `file_mgr_run_job()` / `file_mgr_run_jobs()` - 不持任何模块锁执行 `fopen` / `fwrite` / `fflush` / `fclose` / sidecar 与目录清理
- `file_mgr_get_handle()` / `_write()` / `_close()` / `_attach_meta()` / `_is_tracking()` - 旧签名薄兼容层，**仅供单线程测试夹具**，生产路径禁止调用
- `file_mgr_cleanup()` - 清理旧文件；首次由写线程执行，此后每完成 8 个会话再跑一次

### 5.4 日志模块（logger.c）

**设计思路**：提供日志抽象层

```c
typedef enum {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO
} log_level_t;

// 日志输出接口
void log_message(log_level_t level, const char *fmt, ...);

// Android 平台使用 ALOGX，PC 平台使用 printf
// 通过条件编译或函数指针切换
```

---

## 6. 核心流程

### 6.1 dump_pcm_data_write() 流程

```
1. 首次调用时自动初始化（单例）
2. 检查全局开关（从 property 读取）
3. 检查存储空间
4. 按 key 查找当前会话
5. 记录本次 write 的 call-entry 时间
6. 若已有会话且相邻进入时间差 <=2 秒，继续写同一 session
7. 若已有会话且相邻进入时间差 >2 秒，按隐式 stop 处理，先结束旧会话
8. 若需要创建新会话，则再次检查全局开关、剩余存储空间和并发会话数，检查通过后再创建新会话
9. 拷贝数据到堆内存（data_block_t）并记录 call-entry 时间
10. 加锁，入队，唤醒后台写线程
11. 返回（不阻塞）
```

### 6.2 dump_pcm_data_stop() 流程

```
1. 查找对应的会话
2. 标记会话停止接收新数据
3. 刷新该会话已入队数据
4. 关闭文件句柄
5. 结束该会话生命周期，后续同 key write 重新走新会话
```

### 6.3 后台写线程流程

```
1. 决策段（持 C）：等条件变量 → 取 block → 按身份键找会话 → 做 POD 快照（含 fp 值，仅作身份标签）→ 解锁 C
2. 执行段（不持锁）：capture 出 job（capture 内部短持 F 做槽位查表，返回前释放）→ run 执行
   fopen / fwrite / fflush / fclose / sidecar，全部阻塞 I/O 只发生在这里
3. 发布段（重持 C）：按身份键与 fp 值复核会话 → 发布新句柄 / 文件大小 → pending_blocks--
   → 若会话处于 STOPPING 且已归零则销毁（只摘句柄）→ 解锁 C
4. 锁外收尾：关闭上一步摘下的句柄并写 final sidecar，必要时执行保留期清理
5. 循环，直到 queue 空且 writer_running 归零
```

次序被两条不变量钉住：**F 的最后一次 unlock 严格早于发布段第一次 lock(C)**（L2），以及**任何阻塞 I/O 都不得在持 C 或持 F 的区间内执行**（L4）。细节与静态判据见 [`stage2-lock-order-spec.md`](stage2-lock-order-spec.md)。

---

## 7. 存储空间检查策略

- **检查时机**：每次 `dump_pcm_data_write()` 进入时，以及准备创建新的 dump 会话前
- **总开关**：读取当前 `vendor.audio.dump` 值
- **低存储处理**：低于 500MB 时停止接收新数据，并设置 `low_storage_triggered`
- **恢复条件**：该标志锁存，必须先关总开关再重新打开才清除

---

## 8. 文件管理策略

### 8.1 文件命名

```
/data/vendor/audio_dump/pcm/<escaped-key1>_<escaped-key2>_<stable-hash>_<generation>_<split-index>_<YYYYMMDD_HHMMSS_mmm>.pcm
```

### 8.2 目录创建

- 首次写入前检查目录是否存在
- 不存在则创建（递归创建）

### 8.3 句柄管理

- 最多同时打开 16 个文件句柄
- 空闲超时 5 秒关闭最久未使用的句柄
- 句柄满时关闭最久未使用的

### 8.4 文件分片

- 单文件大小上限 1GB
- 超过后只在同一 session 内关闭当前文件并创建新文件，generation 不变，split-index 加 1

### 8.5 旧文件清理

- 保留最近 100 个文件，或最近 7 天
- 模块初始化时执行清理

---

## 9. 错误处理

| 场景 | 处理方式 |
|------|----------|
| 文件打开失败 | 丢弃数据，记录 ERROR 日志 |
| 目录创建失败 | 丢弃数据，记录 ERROR 日志 |
| 写盘失败 | 丢弃数据，记录 ERROR 日志 |
| 队列满 | 丢弃最新块，记录 WARN 日志 |
| 并发会话超限 | 丢弃数据，记录 WARN 日志 |
| 存储空间不足 | 停止接收，设置 low_storage_triggered 标志 |

---

## 10. PC 测试方案

### 10.1 测试环境

- 使用 `/tmp/audio_dump_test/` 作为 dump 目录
- 通过环境变量 `AUDIO_DUMP_ENABLED=1` 模拟总开关
- 使用 `AUDIO_DUMP_DIR=/tmp/audio_dump_test` 指定目录

### 10.2 测试用例

1. **基本功能测试**：write → stop → 验证文件内容
2. **2 秒会话滚转测试**：相邻 write 进入时间差 ≤2 秒保持同一文件，>2 秒触发隐式 stop 并开新文件
3. **并发测试**：多线程同时 write
4. **队列满测试**：快速写入验证丢弃行为
5. **存储检查测试**：模拟低存储环境与锁存恢复
6. **句柄 LRU 测试**：验证 5 秒 idle close 只影响文件句柄，不影响 session

### 10.3 Makefile 编译

构建清单以文件为准，不在文档里复写：PC 侧见 [`dump_pcm_data/Makefile`](../dump_pcm_data/Makefile) 与 [`CMakeLists.txt`](../dump_pcm_data/CMakeLists.txt)，Android 侧见 [`Android.mk`](../dump_pcm_data/Android.mk)（`LOCAL_SRC_FILES` 为 `src/` 下的 8 个 TU：`logger.c`、`queue.c`、`session.c`、`file_mgr.c`、`dump_control.c`、`dump_writer.c`、`sidecar.c`、`dump_pcm_data.c`）。两处清单与 `ls src/*.c` 一致；阶段二的改动只新增了头文件，未新增编译单元。

---

## 11. 线程安全设计

| 锁 | 变量 | 保护的资源 |
|----|------|------------|
| C | `g_ctx.mutex` | 队列、会话表、`pending_blocks` 记账、`writer_running` |
| F | `g_file_mgr_mutex` | 文件槽位表（`file_info_t` 数组）与保留期清理的槽位摘除 |
| S | `g_storage_mutex` | 存储探测本身：1 秒缓存判定 + 同步的 `statvfs` / `GetDiskFreeSpaceExA` 查询。探测是阻塞调用，因此**不能**放在 C 里，这就是它单独成锁的原因 |
| I | `g_init_mutex` + `g_init_cond` | 惰性单例的初始化状态机 |

**锁序规则**：

- **F 是叶子锁**（L1）：持 F 的路径不得再取 C / S / I，也不得调用可能取它们的函数；每个 `file_mgr_capture_*` 入口（含全部提前返回分支）必须在返回前释放 F。持 C 时取 F 不被禁止，但热路径上不这么做。
- **I/O 不占锁**（L4）：`fopen` / `fwrite` / `fflush` / `fclose` / sidecar 的 open-write-rename / `opendir`-`stat`-`remove` 一律在锁外执行。因此写线程落盘期间，HAL 线程不会因为等在 C 上而阻塞。
- **句柄关闭准入**（L5）：`fclose` 只能由「持 C 且观察到 `pending_blocks == 0`」或「对该 `FILE *` 独占所有权的线程」发起。

**注意**：
- 后台写线程是唯一的消费者
- 多个 HAL 线程是生产者，使用同一把 C
- `dump_pcm_data_write()` 进入时间由 HAL 线程捕获，2 秒会话滚转判断不依赖后台落盘时间
- stop 操作先阻止同 key 新数据进入，再刷新队列并关闭文件；`fclose` 与 final sidecar 由调用线程在解锁后执行（不占锁，但确实在该线程上）
- 上述三条不变量的完整定义、可达性论证与静态/动态判据见 [`stage2-lock-order-spec.md`](stage2-lock-order-spec.md)、[`stage2-lock-order-evidence.md`](stage2-lock-order-evidence.md) 与 [ADR 0002](adr/0002-stage2-lock-order-decisions.md)

---

## 12. 性能考量

| 指标 | 目标 |
|------|------|
| write() 延迟 | < 1ms（不含锁竞争）；实测 p50 ≈ 17 µs、p99 ≈ 30 µs（4000 次调用，Windows / SSD 热缓存），20 ms 落盘停顿注入下调用线程仍 < 300 µs |
| 队列处理延迟 | < 100ms |
| 内存占用 | < 20MB |
| 文件句柄 | < 16 个 |

---

## 13. 编译与集成

### 13.1 编译选项

```makefile
CFLAGS += -std=c11 -Wall -Wextra
CFLAGS += -fPIC -fvisibility=hidden  # 共享库
```

### 13.2 导出符号

```c
// 导出的符号（供 HAL 调用）
__attribute__((visibility("default")))
void dump_pcm_data_write(const char *buffer, uint32_t buffer_size, const char *key1, const char *key2);

__attribute__((visibility("default")))
void dump_pcm_data_stop(const char *key1, const char *key2);
```

### 13.3 Android 集成

- 编译为共享库（BUILD_SHARED_LIBRARY），仅导出公开 API 符号
- 通过 `LOCAL_MODULE := audiohal_pcm_dump` 集成到 HAL

---

## 14. 待确定事项

### 14.1 Android property 读取方式

**已确定**：每次 `dump_pcm_data_write()` 进入时直接读取 `vendor.audio.dump`

- 不缓存总开关状态
- 与存储空间检查时机一致

### 14.2 日志输出具体实现

**已确定**：选项 C - 函数指针注入

```c
// src/logger.h
typedef void (*log_fn_t)(int level, const char *tag, const char *msg);

// 默认实现：空操作
void logger_init(log_fn_t fn);  // 由调用方注入具体实现

// Android 端注入 ALOG 实现，PC 端注入 printf 实现
```

### 14.3 文件保留策略配置化

**已确定**：选项 A - 固定值

- 保留最近 100 个文件，或最近 7 天
- 后续如有需求可再加配置化

---

## 15. 实现顺序建议

1. 基础数据结构定义（src/logger.h, src/queue.h）
2. 队列实现（queue.c）
3. 会话管理（session.c）
4. 文件管理（file_mgr.c）
5. 日志模块（logger.c）
6. 主模块整合（dump_pcm_data.c）
7. PC 测试用例编写
8. PC 测试验证 ✅
9. Android 移植

---

## 16. Android 移植说明

### 16.1 环境适配

| 项目 | PC 环境 | Android 环境 |
|------|---------|--------------|
| dump 目录 | `/tmp/audio_dump_test/` | `/data/vendor/audio_dump/pcm/` |
| 总开关 | 环境变量 `AUDIO_DUMP_ENABLED` | property `vendor.audio.dump` |
| 日志输出 | printf | ALOG (liblog) |

### 16.2 代码修改

**property 读取（dump_pcm_data.c）**：

```c
#ifdef ANDROID
#include <cutils/property.h>
static int check_enabled(void) {
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.audio.dump", value, "0");
    return (strcmp(value, "1") == 0) ? 0 : -1;
}
#endif
```

**日志实现（logger.c）**：

```c
#ifdef ANDROID
#include <android/log.h>
static void android_log_fn(...) {
    __android_log_print(android_level, tag, "%s", msg);
}
void logger_init(log_fn_t fn) {
    g_log_fn = (fn == NULL) ? android_log_fn : fn;
}
#endif
```

### 16.3 Android 编译

创建 `Android.mk`：

```makefile
LOCAL_MODULE := audiohal_pcm_dump
LOCAL_SRC_FILES := \
    src/logger.c \
    src/queue.c \
    src/session.c \
    src/file_mgr.c \
    src/dump_control.c \
    src/dump_writer.c \
    src/dump_pcm_data.c
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -DANDROID -D_POSIX_C_SOURCE=200809L -fvisibility=hidden
include $(BUILD_SHARED_LIBRARY)
```

### 16.4 HAL 集成

在需要 dump 的位置调用：

```c
#include "dump_pcm_data.h"

// out_write() 中
dump_pcm_data_write(buffer, buffer_size, stream_key, "out_primary");

// out_standby() 中
dump_pcm_data_stop(stream_key, "out_primary");
```

### 16.5 验证步骤

```bash
cd dump_pcm_data
make
make run
make coverage
```

### 16.5 使用方法

```bash
# 开启 dump
adb shell setprop vendor.audio.dump 1

# 关闭 dump
adb shell setprop vendor.audio.dump 0

# 查看 dump 文件
adb shell ls -la /data/vendor/audio_dump/pcm/
```
