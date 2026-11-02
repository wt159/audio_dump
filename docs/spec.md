# Android Audio HAL PCM Dump 模块需求说明

## 1. 目标

本模块用于在 Android Audio HAL 中抓取 PCM 原始音频数据并落盘，供调试、问题复现和离线分析使用。

## 2. 调用场景

模块由 Audio HAL 调用：

- `out_write()` 和 `in_read()` 调用 `dump_pcm_data_write()`
- `out_standby()` 和 `in_standby()` 调用 `dump_pcm_data_stop()`

模块内部使用单例模式，首次调用 `dump_pcm_data_write()` 时自动初始化。
调试接口由 AudioFlinger 管理（内部接口，不对外暴露）：
- AudioFlinger 调试时调用 `dump_pcm_data_get_status()` / `dump_pcm_data_get_active_sessions()`

## 3. 核心要求

- 语言必须为 C，标准为 C11。
- 必须异步解耦，不能在 HAL 读写线程中执行磁盘 I/O。
- 模块必须线程安全，可被输入/输出路径并发调用。
- 模块只保存原始 PCM 数据，不做格式转换、重采样、混音或加头封装。

## 4. 运行模式

- 默认采用“手动开关开启”模式。
- 新增全局 dump 总开关，默认关闭；需要 dump 时由上层手动打开，例如通过 Android property `vendor.audio.dump` 控制。
- 总开关关闭时，模块不得开启新的 dump 会话，也不得继续接收新的 dump 数据。
- 一条 dump 流由 `key1 + key2` 唯一标识，其中 `key1` 和 `key2` 都是字符串，最长 64 字节。
- 同一组 `key1 + key2` 在 `write` 和 `stop` 之间视为同一条流；但若同 key 的两次 `dump_pcm_data_write()` 调用进入时间差超过 2 秒，则必须视为前一条流已结束。
- 未调用 `stop` 的连续写入在两次 `dump_pcm_data_write()` 调用进入时间差不超过 2 秒时，始终写入同一文件。
- `dump_pcm_data_stop()` 只关闭与当前 key 匹配的那条流。
- 同一组 key 在停止后再次写入时，视为新的一次 dump 会话。
- 对同一组 key，若两次 `dump_pcm_data_write()` 调用进入时间差超过 2 秒，模块必须按一次隐式 `dump_pcm_data_stop()` 处理：先结束旧会话，再将当前 write 作为新会话的首包写入新文件。
- 每次 `dump_pcm_data_write()` 进入时，以及准备创建新的 dump 会话前，都必须检查总开关状态和剩余存储空间。
- 总开关通过 Android property `vendor.audio.dump` 控制，模块内部在每次 write 时读取当前值。
- 当 dump 目标所在存储的剩余空间低于 500MB 时，模块必须停止接收新的 dump 数据。
- 低存储触发后会锁存，只有在总开关先关闭再重新打开后才会清除该状态。

## 5. 文件与目录

- dump 文件保存到固定根目录：`/data/vendor/audio_dump/pcm/`
- 目录不存在时，模块必须自动创建。
- 文件命名规则：`<escaped-key1>_<escaped-key2>_<stable-hash>_<generation>_<split-index>_<YYYYMMDD_HHMMSS_mmm>.pcm`
- 其中 key1 和 key2 都按原始字符串匹配会话；落盘文件名必须对不安全字符做转义（替换为下划线），确保路径始终限制在 dump 根目录内。
- `stable-hash` 用于辅助区分同名 key 组合，`generation` 表示同一会话的创建代次，`split-index` 表示单会话内的文件分片序号。
- 文件内容必须与传入 buffer 顺序一致，按收到的顺序连续写入。

示例：

`/data/vendor/audio_dump/pcm/out_primary_music_5f2a1c3d_1_0_20260417_103012_123.pcm`

## 6. 实现约束

- 模块内部仅允许一个后台写线程。
- HAL 线程只负责拷贝/入队，不允许等待文件写入完成。
- 队列中保存待写入的数据块（包含 buffer 拷贝 + key 信息 + 数据大小），跨所有会话共享。
- 队列容量：最多同时 pending 800 个数据块，总大小不超过 512MB，超出后丢弃最新进入的数据块，不得阻塞 HAL 线程。
- `stop` 发生时，必须先停止接收该 key 的新数据，再刷新队列并关闭文件。

## 7. 接口定义

```c
void dump_pcm_data_write(const char *buffer, const uint32_t buffer_size, const char *key1, const char *key2);
void dump_pcm_data_stop(const char *key1, const char *key2);
```

### 7.1 dump_pcm_data_write

- `buffer` 指向待 dump 的 PCM 数据。
- `buffer_size` 为字节数，必须按原样写入。
- `key1/key2` 用于定位目标 dump 会话，其中 `key2` 按字符串精确匹配。
- 该接口必须尽快返回，不能因落盘而阻塞调用方。
- 对同一组 `key1/key2`，模块必须记录每次 `dump_pcm_data_write()` 的调用进入时间；若本次调用与上一次调用进入时间差超过 2 秒，则本次调用必须触发一次隐式 stop 语义，并作为新会话的首包进入新文件。
- 上述 2 秒规则以 `dump_pcm_data_write()` 的调用进入时间为准，不以后台线程实际落盘时间为准。

### 7.2 dump_pcm_data_stop

- 停止与 `key1/key2` 匹配的 dump 会话，其中 `key2` 按字符串精确匹配。
- 若对应会话不存在，接口应直接返回，不得报错中断 HAL。
- 该接口应保证同 key 的后续数据不会继续写入当前文件。

## 8. 失败处理

- 文件打开失败、创建目录失败、写盘失败时，不得影响 Audio HAL 正常读写。
- 发生失败时，模块应丢弃本次 dump 数据并记录日志。
- 当剩余存储空间低于 500MB 时，模块应视为 dump 关闭状态并丢弃后续 dump 数据；该状态会锁存，直到总开关重新关闭再打开。
- 接口无返回值，因此所有失败均为 best-effort 处理。

## 9. 可验收行为

- 在总开关关闭时，`write` 不应产生新的 `.pcm` 文件。
- 当剩余存储空间低于 500MB 时，`write` 不应继续落盘。
- 空间恢复到 500MB 及以上后，dump 仍保持关闭，必须重新切换总开关后才能继续。
- 开启同一 key 的 dump 后，在未显式 `stop` 且相邻两次 `write` 的调用进入时间差不超过 2 秒时，应生成同一个 `.pcm` 文件。
- 开启同一 key 的 dump 后，若相邻两次 `write` 的调用进入时间差超过 2 秒，则后一次 `write` 必须开启新的 `.pcm` 文件，且效果等价于对前一会话执行一次隐式 `stop` 后再开始新会话。
- 该文件内容应等于所有输入 buffer 的顺序拼接结果。
- 调用 `stop` 后，文件应关闭，且同 key 的后续写入应开启新文件。
- HAL 读写线程不应因 dump 模块发生明显阻塞。

## 10. 开发与测试

- **PC 环境开发测试**：模块先在 PC 环境下进行开发和单元测试，验证核心逻辑、线程安全、队列行为、存储检查等。
- **Android 实机迁移**：PC 测试通过后，再将代码迁移到 Android 设备上进行实机验证。

## 11. 运行时接口（内部调试用）

模块内部使用单例模式，首次调用 `dump_pcm_data_write()` 时自动初始化。

提供以下运行时查询接口供调试使用：

```c
int dump_pcm_data_get_status(dump_status_t *status);
int dump_pcm_data_get_active_sessions(dump_session_info_t *sessions, int max_count, int *actual_count);
```

### 11.1 dump_pcm_data_get_status

- 查询当前 dump 模块状态，返回全局开关状态、存储空间信息、是否因低存储已停止。
- `status` 结构体包含：`enabled`（总开关）、`low_storage_triggered`（低存储触发标记）、`available_storage_mb`（剩余空间 MB 数）。

### 11.2 dump_pcm_data_get_active_sessions

- 返回当前正在 dump 的会话列表（key1 + key2 + 已写入字节数）。
- 用于调试和诊断。

## 12. key 约束

- `key1`：字符串类型，用于标识 Audio Stream 或 Port，最大长度 64 字节。
- `key2`：字符串类型，用于标识具体会话，最大长度 64 字节。
- 模块内部使用 key1 + key2 组合作为唯一会话标识，不对 key 字符集做额外限制，但落盘文件名会进行转义。

## 13. 资源约束

- 队列容量：最多同时 pending 800 个数据块，总大小不超过 512MB，超出后丢弃最新进入的数据块，不得阻塞 HAL 线程。
- 最大并发会话数：最多同时 16 条 dump 会话，超出后静默丢弃本次数据并记录 WARN 日志。
- 最多同时打开 16 个文件句柄（与最大会话数一致），超出后关闭最久未写入的会话文件。
- 单文件大小上限：1GB，超过后自动新建文件（保持同一 key 会话的连续性）。
- 内存占用：模块总内存使用不超过 20MB。

## 15. 日志规范

- 日志级别：ERROR（模块异常）、WARN（存储空间不足、队列满）、INFO（会话开始/结束、文件创建）。
- 输出方式：使用 Android logcat（LOG_TAG 为 "AudioHALPcmDump"）。
- 日志示例：
  - `[AudioHALPcmDump] INFO: Session started: key1=out_primary, key2=music`
  - `[AudioHALPcmDump] WARN: Low storage: 450MB, dumping stopped`
  - `[AudioHALPcmDump] ERROR: Failed to create directory`

## 16. 文件管理

- 保留策略：默认保留最近 100 个 dump 文件，或最近 7 天（以先到者为准）。
- 自动清理：模块初始化时执行清理，超出保留策略的文件应删除。
- 路径安全：key2 转义规则 —— 将非 `[a-zA-Z0-9_-]` 字符替换为 `_`，防止路径遍历。
