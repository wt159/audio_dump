# D4 元数据 sidecar 设计（草案）

- 关联：Issue #1 的 D4，方案文档 §9 待确认项 2
- 状态：设计草案，未实现
- 前置约束：**Android HAL 的调用方式保持不变**

---

## 1. 硬约束

1. `dump_pcm_data_write(buffer, size, key1, key2)` 与 `dump_pcm_data_stop(key1, key2)` 的
   签名、语义、调用点全部不变。HAL 的 `out_write()/in_read()/out_standby()/in_standby()`
   一行都不用改。
2. 现有行为不回归：字节顺序拼接、文件名规则、2 秒 rollover、低存储锁存、关闭态零副作用。
3. sidecar 默认关闭。不调用新接口、不打开开关时，产出与现在完全一致（只有 `.pcm`）。
4. sidecar 失败绝不影响 PCM 落盘，也不向调用方返回错误。
5. `.pcm` 内容不得有任何头封装（spec §3），元数据只允许旁路存放。

---

## 2. 为什么需要新的元数据入口

模块当前对采样格式一无所知：`write()` 只有 buffer/size/key。裸 PCM 离开格式就是一串字节，
这正是 issue 里"离线分析需要手工指定格式"的根因。

三种可选入口：

| 方案 | 形态 | 评价 |
|---|---|---|
| A. 扩展 write | `dump_pcm_data_write_ex(..., const dump_pcm_format_t *fmt)` | 无时序要求，格式随首包自然到达；旧接口保留为 `fmt=NULL` |
| B. 独立设置函数 | `dump_pcm_set_stream_format(key1, key2, fmt)` | 需要一张按 key 的挂起表，且引入"必须在首包前调用"的时序约束 |
| C. 编译期全局 | `-DDUMP_PCM_SAMPLE_RATE=48000` | 无法支持多流不同格式，不可用 |

**决策：方案 A。** 理由：HAL 侧本来就在 stream 打开时知道格式，如果将来要产出 sidecar，
在已有的 `out_write()` 里把 `stream->config` 的字段填进去即可，不需要新增调用时机；
而方案 B 会引入"设置早于首包"的隐含契约，桌面侧很容易踩空。旧接口保留，行为等价于
`fmt = NULL`。

---

## 3. 接口草案

```c
typedef enum {
    DUMP_PCM_FORMAT_UNKNOWN = 0,
    DUMP_PCM_FORMAT_S16_LE,
    DUMP_PCM_FORMAT_S24_LE,     /* 24 bit in 32-bit container */
    DUMP_PCM_FORMAT_S24_3LE,    /* packed 3 bytes */
    DUMP_PCM_FORMAT_S32_LE,
    DUMP_PCM_FORMAT_FLOAT_LE
} dump_pcm_sample_format_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channel_count;
    uint16_t bits_per_sample;
    dump_pcm_sample_format_t sample_format;
} dump_pcm_format_t;

/* 与 dump_pcm_data_write 相同，额外携带格式；format 为 NULL 时等价于旧接口。 */
DUMP_PCM_API void dump_pcm_data_write_ex(const char *buffer, uint32_t buffer_size,
                                         const char *key1, const char *key2,
                                         const dump_pcm_format_t *format);

/* sidecar 总开关，默认 0。必须在初始化前调用，已初始化返回 -1。 */
DUMP_PCM_API int dump_pcm_set_metadata_enabled(int enabled);
```

格式校验：`sample_rate` 8000–384000，`channel_count` 1–8，`bits_per_sample` ∈ {8,16,24,32}，
`sample_format` 在枚举范围内且与 `bits_per_sample` 自洽（S16↔16、S24/S24_3↔24、S32/FLOAT↔32）。
任一不满足按"格式未知"处理。

**格式只在会话首包捕获**，后续包携带的格式被忽略（同一会话中途换格式应显式 `stop` 后再写，
这符合现有"换流就换 key"的用法）。捕获点是 `session_create`，与 generation 一起固化。

---

## 4. sidecar 的粒度与生命周期

### 4.1 粒度：一个 `.pcm` 文件对应一个 `.json`

原因：1GB 分片会把同一会话拆成多个文件，2 秒 rollover 也会新建文件。元数据必须跟着
**文件**走，否则分析工具拿到第 2 片时无法判断它属于哪一段。

命名：与 `.pcm` 同名，后缀替换为 `.json`。

```
out_primary_music_5f2a1c3d_1_0_20260417_103012_123.pcm
out_primary_music_5f2a1c3d_1_0_20260417_103012_123.json
```

### 4.2 两次写入

| 时机 | 内容 | 目的 |
|---|---|---|
| 文件首次创建（`is_new`） | 格式、key、generation、split_index、`start_time_unix_ms`、`complete=false` | 进程被杀也有元数据 |
| 文件关闭（任何原因） | 追加 `end_time_unix_ms`、`total_bytes`、`complete=true` | 正常结束时的完整信息 |

"任何原因"包括：显式 `stop`、2 秒 rollover、1GB 分片、LRU 句柄回收、模块销毁。

**这一条决定了 sidecar 的落点**：LRU 回收发生在 `file_mgr` 内部，写线程并不知情。因此
sidecar 的关闭钩子必须挂在 `file_mgr_close()`，而不是写线程。实现上让 `file_info_t`
携带一份自包含的元数据副本（keys + 格式 + generation/split_index + 起始时间，约 170 字节），
避免 session 生命周期与文件句柄不一致时的悬垂引用。

### 4.3 `total_bytes` 的语义

等于该文件**截至最近一次关闭**已写入的字节数。若句柄被 LRU 回收后又重开（append），
关闭钩子会再次覆盖该值，因此它总是"最后一次关闭时的准确值"，不会出现偏小却标记 complete
的情况。

### 4.4 原子写

同目录写 `<name>.json.tmp`，`fflush` + `fclose` 后 rename 覆盖：

- POSIX：`rename()`
- Windows：`MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)`

rename 失败只记 WARN 并删除 tmp，不影响 `.pcm`。

---

## 5. JSON 结构

```json
{
  "version": 1,
  "pcm_file": "out_primary_music_5f2a1c3d_1_0_20260417_103012_123.pcm",
  "key1": "out_primary",
  "key2": "music",
  "generation": 1,
  "split_index": 0,
  "sample_rate": 48000,
  "channel_count": 2,
  "bits_per_sample": 16,
  "sample_format": "s16_le",
  "start_time_unix_ms": 1776397812123,
  "end_time_unix_ms": 1776397825871,
  "total_bytes": 1048576,
  "complete": true
}
```

约定：

- `version` 固定 1，后续加字段不改版本，删改字段才升。
- 未关闭的文件没有 `end_time_unix_ms` 与 `total_bytes`，`complete` 为 `false`。
- `sample_format` 用可读字符串，不写枚举值，方便离线工具直接读。
- `key1`/`key2` 按 JSON 规则转义（`"`、`\`、U+0000–U+001F 用 `\uXXXX`）。HAL 的 key 实际是
  ASCII 标识符，转义成本可忽略。
- `pcm_file` 只写文件名，不写绝对路径——目录可能被宿主重定位，写全路径会让文件不可搬迁。

---

## 6. 失败与边界

| 场景 | 行为 |
|---|---|
| 格式未知（旧接口 / 校验失败） | **不生成 sidecar**，与现状完全一致 |
| sidecar 开关关闭 | 不产生任何 `.json`，无额外系统调用 |
| 目录不可写 | 记 WARN，`.pcm` 继续写 |
| 磁盘满 | 同左；sidecar 先写 tmp 失败则直接放弃本次 |
| 进程被 `SIGKILL` | 保留首次写入的 sidecar（`complete=false`），无 total_bytes |
| 分片 | 每片独立 sidecar，`split_index` 递增 |
| rollover | 新会话新 generation，旧 sidecar 在关闭时补全 |
| 关闭时 `pcm` 已被外部删除 | rename 仍成功，sidecar 指向不存在的文件；记录 WARN 由分析工具自行判断 |

---

## 7. 对 Android HAL 的影响（约束验证）

| 关注点 | 结论 |
|---|---|
| 调用点 | `out_write/in_read/out_standby/in_standby` 不改；继续用旧接口 |
| 旧接口语义 | 字节流、文件名、2 秒规则、低存储锁存全部不变 |
| 默认产物 | 只有 `.pcm`，不产生 `.json`（开关默认 0） |
| 运行时开销 | 每个 write 多一次 `atomic_load`（开关判断），可忽略 |
| 符号兼容 | 只新增符号，不修改/删除现有导出；静态库未引用即不链接 |
| 构建 | `Android.mk` 不变；不定义任何新宏 |
| 若厂商要 sidecar | 在 stream 打开处把 `stream->config` 填进 `write_ex`，或保持旧接口继续不产出 |

验收口径：Android `mm` 通过；关掉开关时新旧路径产出的 `.pcm` 字节完全一致（用同一组输入
分别跑旧接口与 `write_ex(fmt=NULL)` 比对）。

---

## 8. 测试计划

| 编号 | 用例 | 断言 |
|---|---|---|
| S-01 | 首包带格式，后续包格式不同 | 会话沿用首包格式；sidecar 只出现一份 |
| S-02 | 正常 stop | `.json` 与 `.pcm` 同名；字段齐全；`complete=true` |
| S-03 | `total_bytes` | 等于 `.pcm` 的实际大小 |
| S-04 | 1GB 分片（用测试钩子把上限调小） | 两片各一份 sidecar，`split_index` 0/1 |
| S-05 | 2 秒 rollover | 新文件新 generation；旧 sidecar 已补全 |
| S-06 | 不调用 stop（模拟被杀） | sidecar 存在、`complete=false`、无 `total_bytes` |
| S-07 | 开关关闭 / 旧接口 | 目录内无 `.json`；`.pcm` 内容与现状一致 |
| S-08 | 目录只读 | `.pcm` 仍写出；有 WARN；无半成品 tmp 残留 |
| S-09 | JSON 转义 | key 含 `"`、`\`、换行时仍可被 `json.loads` 解析 |

S-02~S-06 依赖真实文件写入，走现有 PC 测试目录；S-09 可用纯函数单测（序列化器）。

---

## 9. 实施拆分

| 批次 | 内容 | 说明 |
|---|---|---|
| D4-A | `dump_pcm_format_t` + `dump_pcm_data_write_ex()` + 会话首包捕获 | 不加 sidecar，行为对旧接口完全等价；单测覆盖格式校验与 `fmt=NULL` 等价性 |
| D4-B | sidecar 模块（序列化 + 原子写）+ `file_mgr` 关闭钩子 + 开关 | 含 `file_info_t` 元数据副本 |
| D4-C | 文档、AGENTS、S-01~S-09 全量用例 | — |

D4-A 可以独立合入，风险最低；D4-B 才真正改变行为，且默认关。

---

## 10. 待确认

1. `dump_pcm_set_metadata_enabled()` 是否也需要"初始化前调用"的限制——若允许运行时开关，
   需要明确已在写的文件是否补写首版 sidecar（当前设计：只在文件打开时判断）。
2. key 若含非 UTF-8 字节，JSON 将不合法。是否改为 base64 编码（可读性下降但绝对安全）。
3. 是否需要 `dump_pcm_metadata_path_for()` 之类的查询接口，方便宿主 UI 直接定位 sidecar。
