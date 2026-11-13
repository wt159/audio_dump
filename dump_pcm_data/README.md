# Android Audio HAL PCM Dump 模块

用于在 Android Audio HAL 或 PC 测试环境中抓取 PCM 原始音频数据并异步落盘。

## 项目结构

```text
dump_pcm_data/
├── include/
│   └── dump_pcm_data.h           # 对外接口、格式结构与状态查询结构体
├── src/
│   ├── dump_pcm_data.c           # 运行时入口、开关、根目录、会话创建
│   ├── dump_control.c            # 初始化、开关判断、存储检查、状态查询
│   ├── dump_writer.c             # 写线程三段式：决策 / 执行 / 发布
│   ├── dump_internal.h           # 内部上下文、常量
│   ├── dump_test_hooks.h         # 仅测试使用的注入点（不安装）
│   ├── file_mgr.c                # 槽位表、capture/run job、清理
│   ├── file_mgr.h
│   ├── file_mgr_job.h            # POD job 与会话快照（仅内部）
│   ├── sidecar.c                 # 元数据 JSON 序列化与原子写
│   ├── sidecar.h
│   ├── session.c                 # 会话、格式捕获、文件动作决策
│   ├── session.h
│   ├── key_utils.h               # key 校验、转义、哈希
│   ├── logger.c
│   ├── logger.h
│   ├── platform_config.h         # 平台配置入口
│   ├── platform_config_default.h # Android 默认配置
│   ├── platform_config_pc.h      # PC 测试配置
│   ├── queue.c
│   └── queue.h
├── test/
│   ├── main.c                    # PC 测试入口（用例数由 _Static_assert 钉住）
│   ├── whitebox.c                # 白盒覆盖补充
│   ├── proc_iso.c                # 隔离用例的子进程执行（唯一 spawn 点）
│   ├── test_plat.h               # 测试侧平台拼写（mkdir / setenv / 测试根）
│   ├── windows_smoke.c           # Windows 运行期冒烟用例
│   ├── header_*.c                # 公开头契约的三份编译单元（可见性 / 观测门控）
│   ├── run_windows_smoke.sh      # Windows 冒烟
│   ├── run_full_windows.sh       # Windows 全量套件
│   ├── check_header_contract.sh  # 公开头契约
│   ├── check_shared_library_exports.sh
│   ├── check_verification_flow.sh    # 汇总全部契约脚本
│   ├── check_lock_order.sh       # 阶段二锁序静态判据（L1 / L2 / L4 / L5）
│   ├── check_blocking_probe.sh   # §13.2 探针常量与构造契约
│   └── check_docs_build_contract.sh
├── tools/
│   ├── lock_block_probe.c        # §13.2 阻塞放大探针（独立二进制）
│   └── run_blocking_probe.sh     # 编译 + 运行 + 清理本次临时树
├── Android.mk                    # Android 编译文件
├── CMakeLists.txt                # CMake 静态库，可 opt-in 观测接口
├── Makefile                      # PC 编译 / 运行 / 覆盖率
└── .gitignore
```

## 对外接口

头文件：`include/dump_pcm_data.h`

```c
/* 写入：沿用旧接口，不携带格式 */
void dump_pcm_data_write(const char *buffer, uint32_t buffer_size,
                         const char *key1, const char *key2);

/* 写入并携带采样格式；format 为 NULL 时与旧接口完全等价 */
void dump_pcm_data_write_ex(const char *buffer, uint32_t buffer_size,
                            const char *key1, const char *key2,
                            const dump_pcm_format_t *format);

void dump_pcm_data_stop(const char *key1, const char *key2);

/* 运行时控制 */
void dump_pcm_set_enabled(int enabled);          /* 1 开 / 0 关 / -1 回落平台默认 */
int  dump_pcm_set_root_dir(const char *dir);     /* 初始化前调用，成功返回 0 */
int  dump_pcm_set_metadata_enabled(int enabled); /* sidecar 开关，默认关闭 */

/* 观测接口：默认构建不导出，需定义 DUMP_PCM_INTERNAL_DEBUG_API
 * 或 DUMP_PCM_DESKTOP_OBSERVE */
/* dump_status_t 的 dropped_blocks 是进程启动以来调用路径丢弃的 block 数：
 * 低存储闩锁、队列满、无可用会话。模块关闭期间的写入不算丢弃。
 * 它是评估「丢得更多但更及时」这类取舍的唯一依据。 */
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
| `format` | 采样格式；仅在会话首包生效，`NULL` 表示未知 |

### 格式结构

```c
typedef enum {
    DUMP_PCM_FORMAT_UNKNOWN = 0,
    DUMP_PCM_FORMAT_S16_LE,
    DUMP_PCM_FORMAT_S24_LE,   /* 24 bit，装在 32 bit 容器里 */
    DUMP_PCM_FORMAT_S24_3LE,  /* 紧密排列的 3 字节 */
    DUMP_PCM_FORMAT_S32_LE,
    DUMP_PCM_FORMAT_FLOAT_LE
} dump_pcm_sample_format_t;

typedef struct {
    uint32_t sample_rate;      /* 8000 – 384000 */
    uint16_t channel_count;    /* 1 – 8 */
    uint16_t bits_per_sample;  /* 与 sample_format 自洽 */
    dump_pcm_sample_format_t sample_format;
} dump_pcm_format_t;
```

校验不通过或传 `NULL` 时，格式被视为未知：不影响写入，只是不生成 sidecar。

### 运行时控制

- `dump_pcm_set_enabled()` 是 push 式开关，优先级高于 Android 属性 / PC 环境变量；传 `-1` 回落到平台默认源。由关变开时会清除低存储锁存。
- `dump_pcm_set_root_dir()` 必须在模块初始化前调用，已初始化时返回 `-1` 且不改变目录。Windows 下反斜杠会被归一化为 `/`。
- `dump_pcm_set_metadata_enabled()` 可在运行中切换，判定发生在文件打开时：开启后新打开的文件有 sidecar，已打开的不补写。

### 状态查询

- `dump_pcm_data_get_status()` 返回 dump 是否开启、低存储锁存状态、缓存的可用空间、队列积压数量和活跃会话数。
- `dump_pcm_data_get_active_sessions()` 返回当前活跃会话的 `key1/key2`、已写入字节数，以及首包捕获到的格式（`has_format` / `format`）。

## 运行时行为

### 开关控制

- Android 通过属性 `vendor.audio.dump` 控制，值为 `1` 时开启。
- PC 测试通过环境变量 `AUDIO_DUMP_ENABLED=1` 控制。
- 两者都可以被 `dump_pcm_set_enabled()` 覆盖：`1` 强制开、`0` 强制关、`-1` 退回属性 / 环境变量。
- dump 从关闭切回开启时，会清除低存储锁存状态。

### 落盘目录

- Android 默认目录：`/data/vendor/audio/`
- PC 默认目录：`/tmp/audio_dump_test/`
- PC 测试程序支持用 `AUDIO_DUMP_DIR` 覆盖输出目录。
- 宿主可在初始化前调用 `dump_pcm_set_root_dir()` 指定任意目录；目录不存在时会逐级创建。

### 会话生命周期

- 会话由字符串 `key1 + key2` 标识。
- 同一组 key 连续写入时，如果两次 `dump_pcm_data_write()` 的调用进入时间差 `<= 2` 秒，则继续写入同一会话。
- 如果时间差 `> 2` 秒，模块会为该 key 新建一代会话，并把旧会话按停止态处理。
- 显式调用 `dump_pcm_data_stop(key1, key2)` 后，该 key 的后续下一次写入会进入新会话。

### 队列与写线程

- 模块内部只有一个异步写线程。
- 写线程把 `fopen` / `fwrite` / `fflush` / `fclose` 与 sidecar 写入全部放在模块锁之外执行：音频线程在锁内只做入队与记账，不会因为写线程正在落盘而在锁上排队。**例外**：发生 2 秒 rollover 或调用 `dump_pcm_data_stop()` 时，`fclose` 与 final sidecar 由调用线程在解锁之后自己执行——不占锁，但确实发生在该线程上。
- Android 默认最多允许 `16` 个活跃会话，队列上限为 `800` 个 block / `512 MiB`。
- PC 测试默认最多允许 `4` 个活跃会话，队列上限为 `8` 个 block / `64 KiB`。
- 当 dump 未开启、低存储锁存、队列已满或会话无法创建时，当前 block 会被丢弃。

### 存储保护

- 模块每秒最多检查一次可用空间。
- 低存储阈值为 `500 MiB`。
- 一旦触发低存储，模块会锁存拒绝后续写入，直到 dump 被先关闭再重新开启。

### 首次写入的开销（实时线程注意）

模块是惰性初始化的：`dump_pcm_set_enabled(1)` 之后，**第一次**满足条件的写入才会真正初始化——逐级建目录、探测可用空间、创建写线程。这些都在**调用线程**上完成。

本机实测（Windows / SSD / 热缓存，4000 次调用）：第一次 `dump_pcm_data_write()` 约 `13 ms`，之后 p50 `17 µs`、p99 `30 µs`。若调用方就是音频实时线程，这十几毫秒够造成一次欠载。

建议的集成方式：在 HAL 初始化阶段（非实时线程）把开关打开，用一个不会重现的假 key 写一个 block，再把开关切回。初始化每个进程只做一次，之后开关切换不会重复这个开销——但注意开关关闭时写入会被直接丢弃，预热必须发生在开关打开的状态下。

### 文件切分与清理

- 单文件大小上限为 `1 GiB`，超过后会在同一会话内递增 `split-index` 新建分片文件。
- 历史文件清理由写线程执行（不在调用线程做）：写线程启动时清理一次，之后每完成 `8` 个会话再清理一次——只在启动时清理的话，长时间运行的宿主永远不会再淘汰过期文件。仅当 `.pcm` 文件总数超过 `100` 时，删除超过 `7` 天的旧文件；只匹配 `.pcm` 后缀，宿主自己的 `mix.pcm.bak` 之类不会被误删。

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
- 路径总长受内部 256 字节缓冲限制。放不下时**只裁剪两个 key**，`stable-hash` / `generation` / `split-index` / 时间戳与 `.pcm` 后缀始终保留：保留策略靠后缀识别文件，唯一性靠哈希与代次，裁掉它们会得到既不会被清理、又可能与其它会话撞名的文件。连固定部分都放不下时，该次写入被丢弃并记录 `ERROR`，不会写出半截名字。

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

# 覆盖率棘轮（下降即非 0 退出）
make coverage-ratchet

# 手工运行
AUDIO_DUMP_ENABLED=1 AUDIO_DUMP_DIR=/tmp/audio_dump_test ./dump_pcm_test

# CMake 静态库（-DDUMP_PCM_DESKTOP_OBSERVE=ON 会导出观测接口）
cmake -S . -B build/cmake -DDUMP_PCM_DESKTOP_OBSERVE=ON
cmake --build build/cmake
```

说明：

- `make run` 会创建 `/tmp/audio_dump_test`，并在开启 dump 的状态下执行测试程序。
- `make coverage` 会运行测试并打印 gcov 汇总，命令本身固定以 0 退出。
- `make coverage-ratchet` 读 `make coverage` 存下的 `build/coverage/prod/gcov-summary.txt`，把 8 个生产 TU 的行覆盖率与 `test/coverage_baseline.txt` 按千分比比对（容差 0.5 pp），下降即非 0 退出并打印文件名与两个数字。基线绑定 CI 镜像的 gcc 15，换镜像或换编译器属**重标事件**而非回归。
- 用例清单、断言级别与「确认不测」的理由见 [`../docs/test-matrix.md`](../docs/test-matrix.md)，判据与验收见 [`../docs/test-gap-spec.md`](../docs/test-gap-spec.md)。
- 如果只想复现某些隔离测试，可直接运行 `dump_pcm_test` 并配合对应环境变量。

## Windows 运行期验证

Windows 上现在能跑**完整**的测试套件，不再只是编译通过：

```bash
cd dump_pcm_data
MINGW_BIN=/d/Qt/Tools/mingw730_64/bin sh test/run_full_windows.sh
```

脚本用 MinGW-w64 编译模块与全部测试源码，然后在开启 dump 的状态下执行，末尾应看到 `Tests Failed: 0` 且 `Registered` 与 `Total Tests` 相等。加 `USE_CMAKE=1` 改走 `CMakeLists.txt`：

```bash
USE_CMAKE=1 \
CMAKE_BIN=/d/tools/cmake-3.31.6-windows-x86_64/bin/cmake.exe \
MINGW_BIN=/d/Qt/Tools/mingw730_64/bin \
sh test/run_full_windows.sh
```

也可以把测试目标交给 CMake 与 CTest（两平台通用）：

```bash
cmake -S . -B build/win -G "MinGW Makefiles" -DDUMP_PCM_BUILD_TESTS=ON
cmake --build build/win
ctest --test-dir build/win --output-on-failure
```

`make run` 的 `fork/exec` 已经不再是问题：需要干净进程的用例统一走 `test/proc_iso.c`（POSIX 用 `fork` + `execve`，Windows 用 `CreateProcessA`）。`make` 本身仍依赖 POSIX 工具，所以 Windows 上用上面两条命令而不是 `make run`。

另有一个十秒级的快速冒烟，不依赖测试夹具，覆盖 Windows 专有的运行期路径：

```bash
MINGW_BIN=/d/Qt/Tools/mingw730_64/bin sh test/run_windows_smoke.sh
```

它覆盖：

- `_mkdir` 逐级建目录，以及反斜杠路径归一化
- `GetDiskFreeSpaceExA` 的可用空间探测（断言高于 500 MiB 阈值）
- 写线程、会话创建与文件落盘
- sidecar 的 `MoveFileExA` 原子替换与 `complete` / `total_bytes`
- 初始化后再调 `dump_pcm_set_root_dir()` 被拒绝

`MINGW_BIN` 指向带 winpthreads 的工具链目录；默认取本机的 `D:/Qt/Tools/mingw730_64/bin`。全量套件挂了时先跑它，可以快速区分是模块坏了还是夹具坏了。

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

> HAL 的以上调用方式不需要改动。若确实需要元数据 sidecar，可把第 2 步换成
> `dump_pcm_data_write_ex()` 并传入 `stream->config` 中的采样率、声道、位深与格式，
> 同时调用 `dump_pcm_set_metadata_enabled(1)`。不开启时不会产生任何 `.json`，
> 旧调用的字节输出也完全不变。

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

## 元数据 sidecar

默认关闭。开启后，每个 `.pcm` 文件旁边会有一个同名 `.json`：

```text
out_primary_music_5f2a1c3d_1_0_20260417_103012_123.pcm
out_primary_music_5f2a1c3d_1_0_20260417_103012_123.json
```

生成需要同时满足三个条件：

1. `dump_pcm_set_metadata_enabled(1)` 已开启；
2. 文件打开时开关处于开启状态（已打开的文件不补写）；
3. 该会话首包通过 `dump_pcm_data_write_ex()` 携带了合法格式。

行为要点：

- 文件创建时写首版（`complete: false`，没有 `total_bytes`）；关闭时补全 `total_bytes` / `end_time_unix_ms` 并置 `complete: true`。
- "关闭"覆盖显式 stop、2 秒 rollover、1 GiB 分片、句柄 LRU 回收和模块销毁。
- 进程被强杀时保留首版，`complete: false` 可让分析工具判断该文件不完整。
- 写入采用 `<name>.json.tmp` + rename 的原子替换；失败只记 WARN，不影响 `.pcm`。
- 旧接口不携带格式，因此不会产生任何 `.json`。
- 完整字段与失败矩阵见 [`docs/d4-sidecar-design.md`](../docs/d4-sidecar-design.md)。
