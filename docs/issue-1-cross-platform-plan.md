# 跨平台接入实施方案（Issue #1）

- 关联：GitHub Issue #1《跨平台接入需求：Windows/MinGW 构建、运行时开关与关闭态零副作用》
- 状态：方案已落盘，待评审；代码未改动
- 日期：2026-11-08
- 验证基线：`cd dump_pcm_data && make && make run && make coverage`

---

## 1. 背景与目标

接入方 SoundBridge（C++11 + Qt5，Windows/MinGW 7.3 + Linux，CMake）希望复用本模块做样本级排障。
本方案的目标不是"让它在 Windows 上编译过"，而是：

1. **明确并兑现对调用线程的承诺**：不阻塞、不做磁盘 I/O、关闭态零副作用。
2. 在不改变既有核心语义的前提下，完成 MinGW/Linux 桌面接入。
3. 每一步都可独立验证、独立回滚，Android HAL 行为不回归。

### 1.1 与 Issue 的三点判断差异

| 项 | Issue 的建议 | 本方案的判断 |
|---|---|---|
| B3 | setclock 失败则降级为默认时钟并继续 | 方向对，但**必须同时改 `dump_writer.c` 的超时时钟来源**，否则引入 100% CPU 空转 |
| D1 | check_enabled 前移到 init 之前 | 采纳，但它只解决"关闭态"；**开启态首次 write 的磁盘 I/O 是本方案新增的 C1** |
| D2 | 注入 `dump_enabled_query_fn_t` 回调（pull） | 改为 **push 式 `dump_pcm_set_enabled()`**，避免每次音频回调调用宿主函数 |

---

## 2. 范围

### 2.1 本期做

| 编号 | 内容 | 来源 |
|---|---|---|
| B1 | 公开头去除 `<sys/statvfs.h>` 依赖，存储探测平台抽象 | Issue |
| B2 | `mkdir(path, mode)` 平台宏化 | Issue |
| B3 | `pthread_condattr_setclock` 失败降级 + 写线程时钟统一 | Issue（方案修正） |
| D1 | 关闭态零副作用：不初始化、不建目录、不建线程、不删文件 | Issue |
| D2 | 运行时开关（push 式） | Issue（方案修正） |
| D3 | 根目录运行时配置公开化 + 路径安全 | Issue（范围扩大） |
| D5 | CMake 构建目标 + MinGW 编译 | Issue |
| C1 | 开启态首次 write 的磁盘 I/O 下沉到写线程 | 本方案新增 |
| C2 | `file_mgr_cleanup` 文件名匹配收紧为 `.pcm` 后缀 | 本方案新增，D3 的前置条件 |
| C3 | 未初始化时状态查询接口的语义定义 | 本方案新增，D1 的连带影响 |

### 2.2 本期不做

- **D4 元数据 sidecar**：另立项。根本缺口是模块不知道采样格式，`write()` 没有元数据入口，
  需要先定义新的元数据输入契约（见 §9）。
- **D6 默认导出观测接口**：不推翻 Android 共享库现有导出契约。改为桌面构建可选开启（见 D-08）。

### 2.3 明确保持不变

`void` best-effort 契约；有界队列满则丢包、调用方永不阻塞；HAL 调用位置；
2 秒空档隐式 rollover；低存储 500MB 锁存及"先关再开"清除语义；`MAX_SESSIONS`；
文件名规则与目录转义；不做格式转换/重采样/混音；`Makefile` 与 `Android.mk` 现有目标不变。

---

## 3. 关键设计决策

### D-01 条件变量时钟必须有单一事实来源（B3）

**问题**：`src/dump_pcm_data.c:75` setclock 失败即 init 失败；但若只在这里降级，
`src/dump_writer.c:59` 仍用 `CLOCK_MONOTONIC` 计算绝对超时。cond 退回 realtime 后，
该 abstime 被解释为 1970 年的时间，永远已过期 → `pthread_cond_timedwait` 立即返回
→ 空队列时写线程忙等，100% CPU 并反复争抢 `g_ctx.mutex`。

**决策**：在 `dump_context_t` 增加 `clockid_t cond_clock_id`。

- init：`pthread_condattr_setclock(..., CLOCK_MONOTONIC)` 成功 → 记 `CLOCK_MONOTONIC`；
  返回 `EINVAL`/`ENOTSUP` → 打 WARN，不销毁已建资源，改用默认 attr 继续，记 `CLOCK_REALTIME`。
- 写线程：`clock_gettime(g_ctx.cond_clock_id, &ts)` 计算 deadline。
- 除上述两种情况外的初始化失败仍按原路径返回 -1。

**不变量**：cond 的时钟与 writer 计算 deadline 的时钟必须来自同一个变量，禁止再出现字面量时钟 ID。

### D-02 总开关门控初始化（D1）

`dump_pcm_data_write()` 的判断顺序调整为：

```
check_enabled()  →  参数校验  →  ensure_initialized()  →  check_storage()  →  加锁入队
```

- 关闭时直接返回，不触发 `do_init_once()`，因此不建线程、不建目录、不执行 `file_mgr_cleanup`。
- `check_enabled()` 只读写 `g_ctx` 中的 atomic 字段，静态零初始化即可安全先于 init 调用。
- 加锁后的 `atomic_load(&g_ctx.enabled)` 二次检查保留不变。
- `dump_pcm_data_stop()` 行为不变（未 READY 直接返回）。

### D-03 调用线程零磁盘 I/O（C1，本期新增）

现状：开启态首次 write 在调用线程上执行 `ensure_dir_exists`（stat/mkdir）、
`file_mgr_cleanup(100, 7)`（opendir + 全量 stat + O(n²) 排序 + remove），
与 `docs/spec.md` §3「不能在 HAL 读写线程中执行磁盘 I/O」冲突。

**决策**：新增 `file_mgr_prepare()`，由写线程启动后、进入消费循环前执行，内容为
"创建根目录 + 执行保留策略清理"。`do_init_once()` 不再做这两件事，只负责
互斥量/条件变量/队列/会话管理/线程创建。

**失败语义**：`file_mgr_prepare()` 失败时打 ERROR，写线程进入"落盘不可用"状态：
**继续消费并丢弃队列数据**（避免队列涨满后反而影响内存占用），不落盘、不重试目录扫描，
调用方无任何感知。后续是否周期性重试由 §9 待确认项决定。

**不在本期**：`check_storage()` 的 `statvfs` 完全下沉。它目前带 1 秒缓存，下沉需要
跨线程请求-应答或周期任务，改动面大，列为 §9 待确认项。

### D-04 运行时开关采用 push 式（D2）

```c
/* enabled: 1 开启，0 关闭，-1 恢复默认来源（Android property / PC 环境变量） */
DUMP_PCM_API void dump_pcm_set_enabled(int enabled);
```

- 内部以 `atomic_int g_enabled_override` 保存，默认 `-1`。
- `check_enabled()` 先读 override，>= 0 时优先采用；否则走原有 property/env 分支。
- **0→1 上升沿清除低存储锁存的逻辑集中在 `check_enabled()` 内**，push 与默认来源共用，
  保证 spec §4 的锁存语义不被新入口绕过。
- 不引入每包回调。宿主要在 UI 切换时调用一次即可，无需保证回调的实时安全性。

### D-05 根目录公开化与路径安全（D3 + C2）

```c
/* 必须在模块初始化前调用；已初始化返回 -1，不改目录。成功返回 0。 */
DUMP_PCM_API int dump_pcm_set_root_dir(const char *dir);
```

- 内部复用 `file_mgr_set_root_dir()`，增加 `g_init_state == DUMP_INIT_UNINITIALIZED` 前置检查；
  D1 落地后"初始化前"等价于"第一次开启态 write 之前"，需在文档中明确写出。
- 统一路径分隔符：Windows 下接受 `\` 并规范化为 `/`；`ensure_dir_exists` 需处理盘符
  （`C:/...`）与跳过已为根的路径前缀，UNC 路径本期不支持并在文档中声明。
- **C2 前置修复**：`file_mgr_cleanup` 由 `strstr(d_name, ".pcm")` 改为后缀精确匹配 `.pcm`，
  避免开放任意根目录后误删 `*.pcm.bak`、`*.pcmx` 等宿主文件。
- 桌面构建不假设 `/tmp` 存在：Windows 默认 `./audio_dump/`，Linux 沿用 `/tmp/audio_dump_test/`；
  建议宿主显式调用 `dump_pcm_set_root_dir()`。

### D-06 公开头与测试钩子拆分（B1）

根因是测试注入点泄漏进了公开头（连带暴露 `pthread.h`）。拆分而非仅替换 typedef：

| 文件 | 内容 |
|---|---|
| `include/dump_pcm_data.h` | `dump_pcm_data_write/stop`、状态结构体、本期新增的公开接口；不再包含 `<sys/statvfs.h>` |
| `src/dump_test_hooks.h`（不对外安装） | 存储/线程创建/时间/文件大小等 `_for_test` 注入点及类型 |

存储探测抽象：

```c
typedef int (*dump_storage_query_fn_t)(const char *path, uint64_t *available_bytes);
```

- Android/Linux 默认实现用 `statvfs`，MinGW 用 `GetDiskFreeSpaceExA`。
- MB 换算与 500MB 阈值判断保留在 `check_storage()`，平台实现只负责给字节数。
- `dump_pcm_set_statvfs_fn_for_test` 相应更名为 `dump_pcm_set_storage_query_fn_for_test`，
  测试桩同步迁移（见 §6.3）。

### D-07 mkdir 平台宏（B2）

在 `platform_config_*.h` 增加：

```c
#define DUMP_MKDIR(path, mode)   /* POSIX: mkdir(path, mode)；MinGW: _mkdir(path)，忽略 mode */
```

`src/file_mgr.c:84`、`:103` 及测试中的直接调用统一改走宏。

### D-08 观测接口可见性（C3 + D6 折中）

- **语义定义**：未初始化（含 D1 之后"从未开启"）时 `dump_pcm_data_get_status()` 返回 -1，
  含义统一为"模块未启用"，不再区分"初始化失败"与"开关关闭"。文档中写明，
  接入方若要展示"关闭但正常"，应结合自身的开关状态。
- **可见性**：新增编译宏 `DUMP_PCM_DESKTOP_OBSERVE`。桌面/CMake 目标定义它时，
  公开头暴露两个观测接口声明，定义处加 `DUMP_PCM_API`。
- **契约不动**：`test/check_shared_library_exports.sh` 仍以默认参数构建共享库并断言
  两个符号**不导出**，该脚本不修改；`test/check_header_contract.sh` 同理保持
  "默认包含公开头时观测接口不可用"的断言。新增一条桌面观测构建的正向编译用例。

### D-09 构建（D5）

- 新增 `dump_pcm_data/CMakeLists.txt`：`add_library(audio_dump STATIC ...)`，仅编译 `src/*.c`，
  **不带** `-Wl,--wrap=`、coverage 标志与测试源文件；提供 `DUMP_PCM_DESKTOP_OBSERVE` 选项。
- 新增 MinGW 平台配置头，资源上限沿用 PC 档，根目录按 D-05。
- `Makefile`、`Android.mk` 不改动；`AGENTS.md` 的 Runtime knobs 补充 MinGW 说明。

---

## 4. 接口变更清单

| 类型 | 接口 | 说明 |
|---|---|---|
| 新增 | `dump_pcm_set_enabled(int)` | D-04，push 式运行时开关 |
| 新增 | `dump_pcm_set_root_dir(const char *)` | D-05，init 前调用，已初始化返回 -1 |
| 修改 | `dump_pcm_data_get_status()` | 语义补充：未启用统一返回 -1，行为本身不变 |
| 移动 | `dump_pcm_set_statvfs_fn_for_test` 等注入点 | 迁至 `src/dump_test_hooks.h` |
| 更名 | → `dump_pcm_set_storage_query_fn_for_test` | D-06，签名改为字节数出参 |
| 内部新增 | `file_mgr_prepare()` | D-03，写线程执行建目录与清理 |
| 内部新增 | `dump_context_t.cond_clock_id` | D-01 |
| 内部新增 | `DUMP_MKDIR` / `DUMP_PCM_DESKTOP_OBSERVE` | D-07 / D-08 |

---

## 5. 行为变化对照

| 场景 | 现状 | 改后 |
|---|---|---|
| 关闭态首次 write | 建线程、建目录、执行清理 | 直接返回，无任何副作用 |
| 开启态首次 write | 调用线程上建目录 + 扫描清理 | 调用线程只入队；建目录与清理由写线程执行 |
| setclock 返回 EINVAL | init 失败，按 500ms 退避永久重试，不落盘 | 降级 realtime + WARN，正常落盘，写线程不空转 |
| 根目录不可创建 | init 失败 | init 成功，写线程报 ERROR 并丢弃数据 |
| 未初始化查询状态 | 返回 -1 | 返回 -1，但语义明确为"未启用" |
| init 后设置根目录 | 无锁直接改，行为未定义 | 返回 -1，拒绝修改 |
| 目录含 `a.pcm.bak` | 可能被当作 dump 文件删除 | 不纳入清理范围 |

---

## 6. 测试与验收

### 6.0 基线问题（先于一切改动修复）

2026-11-08 在干净检出（HEAD `91ed03f`）上实测发现两处基线问题：

| 编号 | 问题 | 证据 | 处理 |
|---|---|---|---|
| G-01 | `make run` 会在 `check-docs-build-contract` 失败 | `test/check_docs_build_contract.sh:8-10` grep 的是 `../design.md`、`../plan.md`，但实际文档在 `../docs/` 下；`set -eu` 使 grep 返回 2 直接终止 | 已修复：引用改为 `../docs/` 前缀，断言内容不变。修复前退出码 2、修复后通过，已对照验证 |
| G-02 | 本机（Windows + Git Bash）无 `gcc` / `make` / `cc` / `gcov` | `which` 全部未找到 | 验证需在 Linux 或 WSL 环境执行；MinGW 侧验证需要 mingw730_64 工具链 |

G-01 属于既有缺陷，与本方案无关，但不修它后续所有 PR 都无法满足"三件套全绿"的验收门槛，
因此单列为 PR0，先合入再做功能改动。

### 6.1 每次改动必跑

`make` → `make run` → `make coverage`（含四个契约脚本），全绿才可提交。
执行环境为 Linux 或 WSL；Windows 本机仅用于编辑与 MinGW 交叉验证。

### 6.2 新增用例

| 编号 | 用例 | 关键断言 |
|---|---|---|
| T-01 | `AUDIO_DUMP_INIT_FAILPOINT=clock` 注入 EINVAL | init 成功；真实生成 `.pcm` 且内容正确；空闲窗口内 `pthread_cond_timedwait` 调用次数有上界（防空转） |
| T-02 | 关闭态调用 write | 不创建目录、不创建线程、不删除既有文件；`get_status` 返回 -1 |
| T-03 | 开启态首次 write | 通过 `--wrap=mkdir` / `--wrap=opendir` 计数，断言调用线程上目录操作次数为 0；等待写线程后文件正常落盘 |
| T-04 | push 开关运行时切换 | 0→1→0 即时生效；低存储锁存仅在重新开启时清除 |
| T-05 | init 后调用 `dump_pcm_set_root_dir` | 返回 -1，后续文件仍落在原目录 |
| T-06 | 目录混入 `x.pcm.bak` / `y.pcmx` / 正常 `.pcm` | 仅 `.pcm` 参与保留策略 |
| T-07 | MinGW 构建 + 运行 | 编译通过；初始化后真实落盘（直接暴露 B3 类问题） |
| T-08 | 桌面观测宏开启构建 | 观测接口可编译、可链接、可调用 |

### 6.3 需要同步修改的既有资产

- `test/main.c`：`run_isolated_init_failpoint_test()`(:2161) 目前对四个 failpoint 一律断言
  `get_status() == -1`，需把 `clock` 分支拆出并反转为 T-01 的正向断言；
  4 个 `fake_statvfs_*` 桩迁移到字节数接口；新增 mkdir/opendir wrap。
- `test/whitebox.c`：`:35`、`:46` 的符号别名映射同步更名。
- `Makefile`：LDFLAGS 增加 `-Wl,--wrap=mkdir -Wl,--wrap=opendir`（T-03 需要）。
- `AGENTS.md`：Runtime knobs 补 MinGW 说明；新增接口写入"High-signal implementation facts"。
- 不修改：`test/check_shared_library_exports.sh`、`test/check_header_contract.sh`、`Android.mk`。
- 仅修正路径：`test/check_docs_build_contract.sh` 的文档引用（G-01），不断言逻辑。

### 6.4 验收标准

1. PC 三件套全绿，覆盖率不出现非预期下降。
2. T-01 ~ T-06 在 Linux PC 通过（B3 不依赖 Windows 即可回归）。
3. T-07/T-08 在 MinGW 7.3 环境通过。
4. Android `mm` 构建通过，HAL 调用点未改。

---

## 7. 分批实施计划

| 批次 | 内容 | 任务 | 状态 |
|---|---|---|---|
| PR0 | 基线修复 G-01 | 修正 `check_docs_build_contract.sh` 文档路径 | ✅ 已合并；CI `pc-verify` 三件套通过 |
| PR1 | D-01 时钟统一 + D-02 开关门控 + D-03 I/O 下沉 + C3 语义 | 实现 + T-01/T-02/T-03 + 既有 failpoint 用例拆分 | 🔧 D-01 + T-01 已实现；D-02/D-03/C3 待做 |
| PR2 | D-06 拆头与存储抽象 + D-07 mkdir 宏 | 实现 + 测试桩迁移 + C2 匹配收紧 + T-06 | ⏳ |
| PR3 | D-04 开关 + D-05 根目录 + D-09 CMake/MinGW + D-08 观测宏 | 实现 + T-04/T-05/T-07/T-08 + AGENTS.md | ⏳ |
| 另立项 | D4 元数据 sidecar | 先定义元数据输入契约 | ⏳ |

每个 PR 独立可回滚。PR0 最先合入；PR1 不依赖 Windows/MinGW 环境，但需要 Linux 或 WSL 跑三件套。

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 时钟改动引入忙等 | D-01 单一事实来源 + T-01 的 timedwait 次数上界断言 |
| init 时机变化使既有测试假设失效 | PR1 先跑全量测试定位依赖 init 副作用的用例，逐个修正而非放宽断言 |
| 建目录失败从"init 失败"变为"运行期丢弃" | D-03 明确失败语义 + ERROR 日志 + 观测接口可见（桌面宏） |
| Windows 路径处理引入新缺陷 | D-05 限定支持范围（不支持 UNC），盘符用例纳入 T-05 扩展 |
| 契约脚本被误改导致观测接口意外导出 | D-08 保持脚本不变，新增正向用例而非放宽原断言 |

---

## 9. 待确认项

1. **`check_storage()` 的 statvfs 是否完全下沉到写线程**：需要跨线程周期任务，本期保留 1 秒缓存的同步调用。
2. **D4 sidecar 的元数据输入契约**：候选为流打开时新增 `dump_pcm_set_stream_format()`；
   还需定义分片/2 秒 rollover 下 sidecar 的粒度，以及进程被杀时 `total_bytes` 缺失的处理。
3. **观测接口是否需要默认导出**：本期用编译宏满足接入方，若后续要求默认导出，需正式修改两个契约脚本与 design.md。
4. **写线程落盘不可用后的重试策略**：本期不重试，避免在故障目录上反复扫描。

---

## 10. 关键代码位置索引

| 位置 | 内容 |
|---|---|
| `src/dump_pcm_data.c:62-116` | `do_init_once()`，含 setclock(:75)、cleanup(:103)、建线程(:104) |
| `src/dump_pcm_data.c:159-238` | `dump_pcm_data_write()`，init(:166) 先于 enabled(:173) |
| `src/dump_writer.c:56-66` | 写线程等待，:59 用 `CLOCK_MONOTONIC` 算 deadline |
| `src/dump_control.c:26-87` | `check_storage()`，同步 statvfs，1 秒缓存 |
| `src/dump_control.c:89-124` | `check_enabled()`，含低存储锁存上升沿清除 |
| `src/file_mgr.c:52-109` | `ensure_dir_exists`，:84/:103 双参数 mkdir |
| `src/file_mgr.c:316-395` | `file_mgr_cleanup`，:336 使用 `strstr(".pcm")` |
| `include/dump_pcm_data.h:7,40-49` | 公开头的 statvfs 依赖与测试注入点 |
| `test/main.c:283-288,2161-2189` | setclock 故障注入与 init 失败断言 |
