# Android Audio HAL PCM Dump 模块

用于在 Android Audio HAL 或 PC 测试环境中抓取 PCM 原始音频数据并异步落盘。

## 项目结构

```text
dump_pcm_data/
├── include/
│   └── dump_pcm_data.h           # 对外接口与状态查询结构体
├── src/
│   ├── dump_pcm_data.c           # 运行时入口、会话管理、写线程
│   ├── file_mgr.c                # 文件创建、复用、清理
│   ├── file_mgr.h
│   ├── key_utils.h               # key 校验、转义、哈希
│   ├── logger.c
│   ├── logger.h
│   ├── platform_config.h         # 平台配置入口
│   ├── platform_config_default.h # Android 默认配置
│   ├── platform_config_pc.h      # PC 测试配置
│   ├── queue.c
│   ├── queue.h
│   ├── session.c
│   └── session.h
├── test/
│   ├── main.c                    # PC 测试入口
│   └── whitebox.c                # 白盒覆盖补充
├── Android.mk                    # Android 编译文件
├── Makefile                      # PC 编译 / 运行 / 覆盖率
└── .gitignore
```

## 对外接口

头文件：`include/dump_pcm_data.h`

```c
void dump_pcm_data_write(const char *buffer, uint32_t buffer_size,
                         const char *key1, const char *key2);
void dump_pcm_data_stop(const char *key1, const char *key2);

int dump_pcm_data_get_status(dump_status_t *status);
int dump_pcm_data_get_active_sessions(dump_session_info_t *sessions,
                                      int max_count, int *actual_count);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `buffer` | PCM 原始数据 |
| `buffer_size` | 数据大小，单位字节 |
| `key1` | 会话主标识，必须是合法非空字符串，最大 64 字节 |
| `key2` | 会话次标识，必须是合法非空字符串，最大 64 字节 |

### 状态查询

- `dump_pcm_data_get_status()` 返回 dump 是否开启、低存储锁存状态、缓存的可用空间、队列积压数量和活跃会话数。
- `dump_pcm_data_get_active_sessions()` 返回当前活跃会话的 `key1/key2` 和已写入字节数。

## 运行时行为

### 开关控制

- Android 通过属性 `vendor.audio.dump` 控制，值为 `1` 时开启。
- PC 测试通过环境变量 `AUDIO_DUMP_ENABLED=1` 控制。
- dump 从关闭切回开启时，会清除低存储锁存状态。

### 落盘目录

- Android 默认目录：`/data/vendor/audio/`
- PC 默认目录：`/tmp/audio_dump_test/`
- PC 测试程序支持用 `AUDIO_DUMP_DIR` 覆盖输出目录。

### 会话生命周期

- 会话由字符串 `key1 + key2` 标识。
- 同一组 key 连续写入时，如果两次 `dump_pcm_data_write()` 的调用进入时间差 `<= 2` 秒，则继续写入同一会话。
- 如果时间差 `> 2` 秒，模块会为该 key 新建一代会话，并把旧会话按停止态处理。
- 显式调用 `dump_pcm_data_stop(key1, key2)` 后，该 key 的后续下一次写入会进入新会话。

### 队列与写线程

- 模块内部只有一个异步写线程。
- Android 默认最多允许 `16` 个活跃会话，队列上限为 `800` 个 block / `512 MiB`。
- PC 测试默认最多允许 `4` 个活跃会话，队列上限为 `8` 个 block / `64 KiB`。
- 当 dump 未开启、低存储锁存、队列已满或会话无法创建时，当前 block 会被丢弃。

### 存储保护

- 模块每秒最多检查一次可用空间。
- 低存储阈值为 `500 MiB`。
- 一旦触发低存储，模块会锁存拒绝后续写入，直到 dump 被先关闭再重新开启。

### 文件切分与清理

- 单文件大小上限为 `1 GiB`，超过后会在同一会话内递增 `split-index` 新建分片文件。
- 初始化时会执行一次历史文件清理：仅当 `.pcm` 文件总数超过 `100` 时，删除超过 `7` 天的旧文件。

## 文件名格式

落盘文件名格式：

```text
<escaped-key1>_<escaped-key2>_<stable-hash-hex>_<generation>_<split-index>_<YYYYMMDD_HHMMSS_mmm>.pcm
```

说明：

- `escaped-key1` / `escaped-key2`：非法路径字符会被转义为 `_`
- `stable-hash-hex`：`key1 + key2` 的稳定哈希，使用 8 位十六进制输出
- `generation`：同一组 key 的会话代次，显式 stop 或超时 rollover 时递增
- `split-index`：同一会话内因文件大小超限产生的分片序号
- 时间戳精确到毫秒

## PC 环境编译与验证

```bash
cd dump_pcm_data

# 编译
make

# 冒烟运行
make run

# make run 的别名目标
make test

# 覆盖率输出
make coverage

# 手工运行
AUDIO_DUMP_ENABLED=1 AUDIO_DUMP_DIR=/tmp/audio_dump_test ./dump_pcm_test
```

说明：

- `make run` 会创建 `/tmp/audio_dump_test`，并在开启 dump 的状态下执行测试程序。
- `make coverage` 会运行测试并打印 gcov 汇总，命令本身固定以 0 退出。
- 如果只想复现某些隔离测试，可直接运行 `dump_pcm_test` 并配合对应环境变量。

## Android 编译

```bash
# 在 Android 源码环境中
cd dump_pcm_data
mm
```

`Android.mk` 当前定义的模块名为 `libaudiohal_pcm_dump`，构建类型为共享库。

## Android 集成

1. 在 HAL 的 `Android.mk` 中添加依赖：

   ```makefile
   LOCAL_SHARED_LIBRARIES += libaudiohal_pcm_dump
   ```

2. 在 `out_write()` / `in_read()` 中调用：

   ```c
   dump_pcm_data_write(buffer, buffer_size, key1, key2);
   ```

3. 在 `out_standby()` / `in_standby()` 中调用：

   ```c
   dump_pcm_data_stop(key1, key2);
   ```

## Android 使用示例

```bash
# 开启 dump
adb shell setprop vendor.audio.dump 1

# 关闭 dump
adb shell setprop vendor.audio.dump 0

# 查看 dump 文件
adb shell ls -la /data/vendor/audio/
```

## 行为示例

- 同一组 `key1 + key2` 连续写入，且相邻两次调用间隔 `<= 2` 秒时，数据会继续追加到同一个 `.pcm` 文件。
- 同一组 `key1 + key2` 连续写入，但相邻两次调用间隔 `> 2` 秒时，后一次写入会创建新的 `generation`，因此落到新的 `.pcm` 文件。
- 显式调用 `dump_pcm_data_stop(key1, key2)` 后，后续同 key 的下一次写入会创建新的 `.pcm` 文件。
- 如果 key 在转义后发生同名碰撞，文件名中的稳定哈希会继续把它们区分开。
