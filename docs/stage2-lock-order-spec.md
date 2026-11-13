# 阶段二实施规格：把文件 I/O 移出 `g_ctx.mutex`

- 状态：**已定稿，可直接交付实施**。地图「阶段二锁层级重构的 C/F 锁序与交叉点」（[#48](https://github.com/wt159/audio_dump/issues/48)，已关闭）的 destination；tracker 上的 canonical 版本在工单「阶段二规格的形状与验收判据」([#51](https://github.com/wt159/audio_dump/issues/51)) 的规格评论里，本文是其入库副本，技术内容一致。
- 基线：`main = 50ab3f8`（阶段一：`file_mgr` 自带静态互斥锁，全部入口自持锁）。本规格不修改 `main`；**代码实施由下游 PR 完成**。
- 依据：工单「锁持有区间全量清单：stop 与写线程的 C/F 每条路径、每个持有点」（[#49](https://github.com/wt159/audio_dump/issues/49)，边集）、「确定性取证：在阶段二草图上证实或否证 F→C 边」（[#52](https://github.com/wt159/audio_dump/issues/52)，实证）、「阶段二锁序不变量与第 4 处耦合的修法选择」（[#50](https://github.com/wt159/audio_dump/issues/50)，决策）、「写线程在锁内 fopen，会以 1.7 倍放大阻塞音频线程」（[#45](https://github.com/wt159/audio_dump/issues/45)，三层重构记录与回退结论）。
- 配套文档：取证过程与复现步骤见 [`docs/stage2-lock-order-evidence.md`](stage2-lock-order-evidence.md)；决策理由与代价清单见 [`docs/adr/0002-stage2-lock-order-decisions.md`](adr/0002-stage2-lock-order-decisions.md)。
- 证据等级：取证是链接生产源码的最小草图，**条件性证实**；本规格以可断言的不变量与回归用例钉住结论，不依赖复现已回退实现。

---

## 1. 目标与非目标

两个目标必须分开验收，不得互相替代：

| 目标 | 内容 | 判据来源 |
|---|---|---|
| G1 防死锁 | 保住 `C→F` 单向 DAG，禁止任何路径产生 `F→C` | 不变量 L1 / L2 + 确定性锁归属用例 |
| G2 防阻塞 | 把所有阻塞型文件 I/O 移出 C，消除音频线程在 C 上的排队 | 不变量 L4 + 20 ms 注入探针 |

非目标：提高 `file_mgr` 吞吐、拆分 C、trylock / timedlock、多写线程 / 无锁化、重定 `g_root_dir` 的锁归属、改 sidecar 内容格式。

---

## 2. 术语表

| 术语 | 定义 |
|---|---|
| **C** | `g_ctx.mutex`（`dump_internal.h:37`）。保护 queue、session 表、`pending_blocks`、会话销毁。 |
| **F** | `g_file_mgr_mutex`（`file_mgr.c:26`，文件内 `static`）。保护 `file_manager_t.files[]` 槽位表。 |
| **S / I** | `g_storage_mutex` / `g_init_mutex`。本阶段二不改它们的语义。 |
| **热路径** | `dump_pcm_data_write_ex()`、`dump_pcm_data_stop()`、写线程一次循环。 |
| **job** | 一个 POD 值对象 `fm_job_t`，自包含一次 I/O 所需的全部数据；不持有任何模块锁，不回引 session。 |
| **capture** | 在 F 的短临界区内做纯内存决策、产出 job；**返回前必须释放 F**。 |
| **run** | 不持任何模块锁地执行 job 里的 I/O；成功后可短暂取 F 做纯内存记账。 |
| **snapshot** | `session_snapshot_t`，决策段在 C 内拷出的会话 POD 快照，供执行段/发布段使用。 |
| **身份键** | `(key1, key2, generation)`，与 `session_find_by_generation()`（`session.c:76-95`）一致。 |
| **发布段** | 写线程重新取 C、把 job 结果写回 session 的那一段。 |
| **兼容层** | 旧 `file_mgr_*` 入口的薄包装，仅供单线程测试夹具；生产路径禁止调用。 |

---

## 3. 不变量（逐字引自「阶段二锁序不变量与第 4 处耦合的修法选择」）

> **L1（F 是叶子锁）** `g_file_mgr_mutex` (F) 为叶子锁：任何持有 F 的路径不得获取 C / S / I，也不得调用任何可能获取它们的函数；F 持有区间内只允许纯内存操作与文件 I/O。每个 `file_mgr_*` 公开入口（含全部提前返回分支）必须在返回前释放 F。持 C / S / I 时获取 F 不被 L1 禁止，但阶段二不得在热路径上这么做。
>
> **L2（发布次序）** 写线程一次处理循环中，「F 的最后一个 unlock」必须严格早于「第一次为发布而取 C」。这是 L1 在操作层面的可断言形式，由确定性并发用例钉死。
>
> **L3（C 不拆分）** queue 入队、session 查找、`pending_blocks` 记账、会话销毁继续由单把 C 串行化。阶段二不引入队列锁 / 会话锁。

本规格**追加一条比 L1 更严的实现约束**：

> **L4（I/O 归属）** 任何阻塞型文件 I/O（`fopen` / `fwrite` / `fflush` / `fclose` / sidecar 的 open-write-rename / `opendir`-`stat`-`remove`）不得在持 C 或持 F 的区间内执行。F 只保护槽位表的纯内存读写；C 只保护 queue / session / 记账的纯内存读写。

> **L5（句柄关闭准入）** 一条会话句柄的 `fclose` 只能由两类主体发起：① 在**持 C** 且观察到 `session->pending_blocks == 0` 时才允许摘除该句柄（`stop_session_locked` 与写线程发布段）；② 对该 `FILE *` 具有**独占所有权**的线程（本线程刚 `fopen` 出、尚未发布的句柄；写线程独占的槽位淘汰；`file_mgr_destroy()` 在 join 写线程之后）。写线程自决策段到发布段之间的在飞 block 由 `pending_blocks >= 1` 钉住，因此外部线程的 stop / rollover 在该窗口内只能把会话置为 `STOPPING`，不能摘除句柄。

L1 是锁序边界（防 G1），L4 是 I/O 归属（防 G2），L5 是句柄生命周期归属（防「capture 出 WRITE job 后，job 在 `run_jobs()` 里 `fwrite` 的同时另一线程 `fclose` 同一 `FILE *`」——这是 L1/L4 都不覆盖的第三类风险）。`file_mgr.c:17-25` 现有注释只禁止 `F→X`、允许 `X→F`，与 L1 一致；阶段二要做的是**保住** L1 并**收紧**到 L4，而不是修一个已存在的环。

---

## 4. 现状与证据摘要

- `50ab3f8` 的边集是 DAG：`C→F` 7 个源码调用点 / 9 条路径、`C→S` 1 个、孤立 F 5 个，**`F→C` = 0**（「锁持有区间全量清单」）。
- 取证 A 变体（发布时仍持 F）→ `ABBA=CLOSED`；B 变体（发布前 `stage2_drop_F()`）→ `ABBA=no`、两线程完成；C 变体（仅 sidecar 注入 20 ms、不停在发布前）→ `caller max 61234 us`、无死锁。故**阻塞放大与 ABBA 是两种现象**，1.7 倍耗时不是死锁证据。
- gdb 归属：会话线程 `dump_pcm_data_stop → stop_session_locked → destroy_session_locked → close_session_file_locked → file_mgr_is_tracking` 持 C 等 F；写线程发布点持 F 等 C。

---

## 5. 新接口

新增内部头 `src/file_mgr_job.h`（不进公开 `include/`），由 `file_mgr.c` 实现。所有类型只依赖 `dump_pcm_data.h` / `sidecar.h` 的既有类型。

### 5.1 POD 类型

```c
typedef enum {
    FM_JOB_NONE = 0,
    FM_JOB_OPEN,          /* fopen(path, "ab") + register slot */
    FM_JOB_WRITE,         /* fwrite + fflush */
    FM_JOB_CLOSE,         /* fclose */
    FM_JOB_INITIAL_META,  /* incomplete sidecar */
    FM_JOB_FINAL_META,    /* complete sidecar */
    FM_JOB_CLEANUP        /* opendir/stat/remove retention sweep */
} fm_job_kind_t;

#define FM_JOB_MAX_PER_STEP 3   /* evict CLOSE + evict FINAL + OPEN */

typedef struct {
    fm_job_kind_t kind;
    char path[256];
    FILE *fp;                       /* owned by this job */
    const char *payload;            /* borrowed; valid until run returns */
    size_t payload_size;
    dump_pcm_file_meta_t meta;      /* six fields, sidecar.h:12-19 */
    uint64_t total_bytes;
    uint64_t end_time_unix_ms;
    bool is_new;
    bool needs_slot_commit;         /* run must update slot->size on success */
    int rc;                         /* set by run */
} fm_job_t;

typedef struct {
    char key1[DUMP_PCM_KEY_MAX_LEN + 1];
    char key2[DUMP_PCM_KEY_MAX_LEN + 1];
    uint32_t generation;
    uint32_t split_index;
    char file_path[256];
    uint64_t file_size;
    uint64_t file_start_unix_ms;
    bool has_format;
    dump_pcm_format_t format;
    FILE *fp_tag;                   /* IDENTITY ONLY, never a write target */
    uint32_t pending_blocks;
    int state;
} session_snapshot_t;
```

`fm_job_t` 必须自包含：`payload` 之外的所有数据在 capture 时从槽位拷出，run 阶段**不得**再访问 `file_manager_t.files[]`。这是 final sidecar 能移出 F 的前提，也符合 `file_mgr.h:18-19` 已注明的「closing hook 不回引 session」。

### 5.2 capture（F 内纯内存，返回前必释放 F）

```c
void file_mgr_capture_open(file_manager_t *mgr,
                           const session_snapshot_t *snap,
                           fm_job_t out[FM_JOB_MAX_PER_STEP], size_t *out_count);
void file_mgr_capture_write(file_manager_t *mgr, FILE *fp,
                            const char *data, size_t size, fm_job_t *out);
size_t file_mgr_capture_close(file_manager_t *mgr, FILE *fp,
                              fm_job_t out[FM_JOB_MAX_PER_STEP]);
void file_mgr_capture_initial_meta(file_manager_t *mgr, FILE *fp,
                                   const dump_pcm_file_meta_t *meta,
                                   fm_job_t *out);
void file_mgr_capture_cleanup(file_manager_t *mgr, int max_files, int max_days,
                              fm_job_t *out);
bool file_mgr_slot_tracked(file_manager_t *mgr, FILE *fp);  /* replaces is_tracking in production */
```

契约：

1. 每个 capture 函数自己 `lock(F)` / `unlock(F)`，**所有提前返回分支**都必须解锁（现有 `file_mgr_get_handle` 有 6 个出口、`attach_meta` 3 个、`cleanup` 4 个，是历史上最容易漏的地方）。
2. capture 区间内**只**做：槽位表读写、`generate_filename()` 的纯字符串构造、`stat` 以外的时钟读取、job 填充。`fopen` / `fclose` / `fwrite` / `fflush` / sidecar 写 / 目录扫描一律不在 capture 内。
3. `file_mgr_capture_open` 在 F 内完成 `tracked` 判定与 `MAX_OPEN_FILES` 淘汰决策；若需淘汰，产出 `FM_JOB_CLOSE` 与（有 meta 时）`FM_JOB_FINAL_META` 两个 job，再产出 `FM_JOB_OPEN`。
4. `file_mgr_capture_close` 在 F 内**摘除槽位**（`in_use=false`、`count--`），并产出 `FM_JOB_CLOSE` 与（有 meta 时）`FM_JOB_FINAL_META`；final job 携带 `slot->size` 与 `sidecar_now_unix_ms()`。**关开关仍要 finalize**（现状 `file_mgr.c:221-222` 故意不检查开关，保持不变）。
5. capture 区间内不得调用 `sidecar_is_enabled()` 之外的任何 sidecar 函数；不得取 C / S / I。

### 5.3 run（不持模块锁，执行 I/O）

```c
int file_mgr_run_job(file_manager_t *mgr, fm_job_t *job);
```

契约：

1. 调用时不得持有 C / F / S / I。`file_mgr_run_job` 按 `job->kind` 执行实际 I/O，把结果写入 `job->rc`。
2. `FM_JOB_WRITE` 成功后，若 `needs_slot_commit`，**短暂取 F** 更新 `slot->size` 与 `last_write_time`（纯内存），随即释放 F；这段 F 临界区不得做 I/O。
3. `FM_JOB_FINAL_META` / `FM_JOB_INITIAL_META` 调用 `sidecar_write_meta()`；`FM_JOB_OPEN` 用 `fopen(path,"ab")` 并在返回后把 `fp` 写回 `job->fp`；`FM_JOB_CLEANUP` 执行 `opendir`/`stat`/`remove`。
4. sidecar 写失败只记 `LOGW`，**不改变 `job->rc` 的成功语义**（与现状 `file_mgr.c:495-497` 一致）。
5. `FM_JOB_WRITE` 的 `payload` lifetime：block buffer 在本次写线程循环结束前有效，run 必须在 `free(block.buffer)` 之前返回。

### 5.4 兼容层（测试夹具专用）

```c
/* Deprecated: test fixtures only. Production paths must use capture/run. */
FILE *file_mgr_get_handle(file_manager_t *, const char *, const char *,
                          uint32_t, uint32_t, char *, size_t, bool *);
int  file_mgr_write(file_manager_t *, FILE *, const char *, size_t);
int  file_mgr_close(file_manager_t *, FILE *);
void file_mgr_attach_meta(file_manager_t *, FILE *, const dump_pcm_file_meta_t *, bool);
bool file_mgr_is_tracking(file_manager_t *, FILE *);
int  file_mgr_cleanup(int, int);
int  file_mgr_init(file_manager_t *);
void file_mgr_destroy(file_manager_t *);
```

实现为 `capture_*()` + `file_mgr_run_job()` 的顺序组合；签名与 `file_mgr.h:29-46` 完全一致，使 `test/main.c` 约 90 处直调继续编译。**生产代码禁止调用兼容层**（由 §12 的静态检查钉住）。

---

## 6. 写线程三段式时序（`writer_thread_func`）

一次循环严格按以下顺序，段与段之间不得合并：

```
[决策段] lock(C)
  pop block; session = session_find_by_generation(key1,key2,generation)
  if session != NULL:
      snap = snapshot(session)          // 含 fp_tag = session->fp（仅身份标签）
      intent = REOPEN / ROLLOVER / USE / CLOSE   // 依据 snap.file_size 与 size limit
  unlock(C)                              // ← C 必须在此释放

[执行段] 不持 C
  jobs = capture_*(snap, intent)          // 每个 capture 内部自持 F、返回前释放
  for each job: file_mgr_run_job(&job)   // 全程不持 C；F 只在纯内存记账时短暂持有
  // 此段结束后，F 的最后一个 unlock 已经发生

[发布段] lock(C)                          // ← L2: 晚于 F 的最后一个 unlock
  session = session_find_by_generation(key1,key2,generation)
  if session == NULL or session->fp != snap.fp_tag:
      enqueue CLOSE/FINAL jobs for the just-opened fp (run AFTER unlock(C))
  else:
      publish job.fp / job.path / file_size; pending_blocks--
      if write failed: do not add file_size
      if state == STOPPING and pending_blocks == 0:
          capture close/final jobs while holding C (pure memory, no F, no I/O)
  unlock(C)
  run any deferred jobs                  // C 外执行，L4
[尾段] maybe_run_retention_cleanup(); free(block.buffer)
```

硬性要求：

- 决策段**不得**调用任何 `file_mgr_*` / `sidecar_*` / `session_file_action()` 中依赖 F 的分支；`session_file_action()` 的 `handle_tracked` 入参改由 `file_mgr_slot_tracked()` 在 capture 内判定并返回。
- 执行段**不得**持有 C；发布段**不得**持有 F。
- `session->fp` 只能在发布段从会话重取（`dump_writer.c:164` 的快照反例必须移除）。
- `pthread_cond_timedwait` 仍在决策段开头（`dump_writer.c:102`），其「带洞」语义不变；MinGW 7.3 上继续回退 `CLOCK_REALTIME`。

---

## 7. stop / shutdown / rollover 的 I/O 迁移

| 路径 | 现状 | 阶段二 |
|---|---|---|
| `dump_pcm_data_stop()`（`dump_pcm_data.c:276-290`） | C 内 `stop_session_locked` → `file_mgr_is_tracking` + `file_mgr_close`（含 final sidecar） | C 内只做标记与纯内存销毁；关文件/final sidecar 拆成「C 内 capture（纯内存，不取 F、不做 I/O）」+「解锁后 run」 |
| rollover 的 `stop_session_locked`（`dump_pcm_data.c:266`，**音频线程**） | 同上，且发生在音频线程 | 同上；这是第二个阻塞注入面，必须一并移出 C |
| `shutdown_module()`（`dump_pcm_data.c:57-61`） | C 内 `file_mgr_destroy` | `pthread_join` 时不持 C 的性质不变；teardown 的 I/O 走 capture/run，且在 C 区间之外 |
| `maybe_run_retention_cleanup()`（`dump_writer.c:71`） | 锁外 `file_mgr_cleanup` | 保持锁外；cleanup 自身也拆成 capture/run，目录扫描与 `remove` 不持 F/C |

`dump_pcm_data_stop()` 的对外语义完全不变：`pending_blocks == 0` 时同步销毁，否则记 `SESSION_STOPPING` 并打日志。不引入延迟销毁与周期扫描。

---

## 8. 身份键与失效处理

**身份键** = `(key1, key2, generation)`，与 `session_find_by_generation()` 一致；该查找不检查 `accepting` / `state`，因此 STOPPING 会话仍能被写线程找到（现状行为，保持）。**次级身份** = `session->fp` 的值（`snap.fp_tag`），只用于发布段比对，绝不作写目标。

| 失效情形 | 处置 |
|---|---|
| 决策段就找不到会话 | 不生成 job，block 直接丢弃（与现状 `dump_writer.c:118-119` 一致）；发布段仍取 C 做 `pending_blocks--` 的兜底 |
| 打开成功但发布段会话已消失 / generation 变化 | 丢弃 job 结果；生成 `FM_JOB_CLOSE`（有 meta 时加 `FM_JOB_FINAL_META`），**在 `unlock(C)` 之后**执行；不得泄漏 fd 或槽位 |
| 会话存在但 `session->fp != snap.fp_tag` | 视为并发 reopen/rollover 换了句柄；同上丢弃并关闭新句柄 |
| stop 在决策段之后、发布段之前到达（`pending > 0`） | 发布段看到 `state == STOPPING`，仍完成记账与 `pending--`；归零时销毁（销毁的 I/O 走 job，C 外执行） |
| `MAX_OPEN_FILES` 淘汰与并发 close 抢同一 fp | `capture_close` 在 F 内摘除槽位后，任何其他 capture 不得再为同一 fp 产出 WRITE job；job 自包含数据，不回引槽位 |

### 8.1 关闭准入与残余窗口可达性（L5 的论证）

三段式把 `fwrite` 放到了「不持 F」的 `run_jobs()` 里，因此必须回答一个问题：**一个已经 capture 出来、尚未 run 的 WRITE job，其目标 `FILE *` 会不会被别的线程关掉？** 若会，就是 `fwrite` 与 `fclose` 对同一 `FILE *` 的并发使用（未定义行为），属比 ABBA 更隐蔽的一类缺陷。逐条枚举生产路径的全部关闭发起者：

| # | 关闭发起者 | 入口 | 准入条件 | 能否与在飞 WRITE job 重叠 |
|---|---|---|---|---|
| 1 | 调用方（`write_ex` 的 2 秒 rollover） | `stop_session_locked` → `destroy_session_locked` | C 内 `pending_blocks == 0` | 否：在飞 block 已计入 `pending_blocks` |
| 2 | 调用方（`dump_pcm_data_stop`） | 同上 | C 内 `pending_blocks == 0` | 同上 |
| 3 | 写线程（发布段） | `destroy_session_locked`（`dump_writer.c:339`） | C 内 `SESSION_STOPPING && pending_blocks == 0` | 否：本 block 先 `pending--` 再判零，后续 block 尚未被 capture |
| 4 | 写线程（执行段 rollover） | `run_deferred_close(snap.fp_tag)` | 独占：本 block 是该句柄唯一写者 | 否：同线程、同一时刻只有一个在飞 block |
| 5 | 写线程（发布段丢弃未发布的新句柄） | `run_deferred_close(pub.fp)` | 独占：刚 `fopen`、从未发布给任何会话 | 否：其他线程不可能持有该指针 |
| 6 | 写线程（`capture_open` 内的槽位淘汰） | `close_idle_file_if_needed` | 写线程独占槽位表 | 否：与 `capture_write`/`run_jobs` 同线程顺序执行 |
| 7 | `shutdown_module` | `file_mgr_destroy` | `pthread_join(writer)` 之后 | 否：写线程已退出 |

**结论**：`file_mgr_capture_write()` 产出的 job 在 `run_jobs()` 里 `fwrite` 的整个区间内，不存在任何生产路径会并发 `fclose` 同一 `FILE *`。窗口不是靠「运气小」排除的，而是靠 `pending_blocks` 记账消除的：它在 C 内 `queue_push` 成功后 +1、只在发布段 C 内 −1，因此「决策段 → 发布段」之间该会话恒有 `pending_blocks >= 1`，而第 1/2/3 条关闭路径全部要求持 C 观察到 0。

因此 [#45](https://github.com/wt159/audio_dump/issues/45) 记录的「跨 C 解锁窗口的 fp 生命周期竞态」**不是可达缺陷**，而是被该记账排除的窗口；ADR 0002 把它列为 Out of scope 的「延迟销毁状态机」也确实不需要——所需的状态已经存在（`pending_blocks` + 两态状态机）。L5 与 §12 判据 9 把这条准入钉在静态检查里，防止后续改动把 `destroy_session_locked` 挪出门控。

**残留的理论边界**（如实记录，不视为缺陷）：`file_mgr_capture_write()` 对**无槽位**的 `fp` 仍会产出 `FM_JOB_WRITE`（保留旧 `file_mgr_write()` 对外来 fp 直接 `fwrite` 的兼容语义，测试夹具有约 90 处直调依赖它）。生产路径不会走到这里——`execute_block()` 的写目标来自会话快照，其槽位由 L5 保证在 run 期间仍有效；只有测试夹具会传入自造 `FILE *`，且夹具是单线程的。

---

## 9. 状态机（不变）

- `SESSION_ACTIVE` / `SESSION_STOPPING` 两态不变（`session.h:14-17`）；`accepting` 语义不变。
- 销毁归属不变：`pending_blocks == 0` 的 stop 在**会话/音频线程**同步销毁；有 pending 时由**写线程**在发布段销毁。
- `pending_blocks` 只在 `queue_push` 成功后 +1（`dump_pcm_data.c:264`），只在发布段 -1；**作业失败也必须 -1**，否则会话永远停在 STOPPING。执行段不得触碰它。
- `dropped_blocks` 仍是调用路径四个丢弃点的计数（producer lost）。阶段二不得新增丢弃点，不得把写线程侧作业失败计入其中。
- 不新增 STOPPED / DESTROYING 态，不新增周期扫描。

---

## 10. 失败路径（逐条）

1. **capture_open 失败**（文件名生成失败 / 槽位满 / `fopen` 失败）：产出 `FM_JOB_NONE`；发布段仍 `pending--`，`session->fp` 保持原值（不置 NULL，避免丢掉可用句柄），记录 `LOGE`；本次 block 丢弃但不计入 `dropped_blocks`（调用方已成功入队）。
2. **写失败**（`fwrite` / `fflush` 失败）：`job->rc != 0`；发布段不增加 `file_size`，不更新槽位 `size`，仍 `pending--`；不重试、不计 `dropped_blocks`。
3. **sidecar 写失败**（initial 或 final）：只记 `LOGW`，不影响 `rc`、不影响 PCM 数据、不影响会话状态；final sidecar 仍按「关开关也 finalize」的规则生成。
4. **打开成功但会话已消失**：见 §8；C 外关闭并（有 meta 时）补 final sidecar。
5. **run 阶段被并发 close 抢先**：由 §5.2(5) 的槽位摘除 + job 自包含保证；implementer 不得在 run 里回读槽位。

---

## 11. 兼容层与测试迁移

- 保留旧 `file_mgr_*` 签名（§5.4），`test/main.c` 的直调、参数守卫、故障注入 setter 与 `test/whitebox.c:54-57` 的宏改名**不改**。
- 67 用例与 `_Static_assert`（`test/main.c:3575`，表达式 67、提示字符串却写 66 的既存不一致）在新增用例时一并修正为实际数量。
- 新增 2 个确定性并发用例（§13）。三份 wrap 列表（`Makefile:7`、`CMakeLists.txt:77-87`、`test/run_full_windows.sh:24-33`）若新增 wrap 符号必须继续一致；`fwrite` / `fflush` 仍走 `file_mgr.h:52-53` 的函数指针钩子，不进 wrap 列表。
- public header 继续不含 `pthread.h` / `sys/statvfs.h`；新头 `file_mgr_job.h` 属 `src/` 内部。

---

## 12. 锁边静态判据

实施后必须能用脚本判定（建议加进 `test/check_verification_flow.sh` 或新增契约脚本）：

1. 生产路径中，`pthread_mutex_lock(&g_ctx.mutex)` 到对应 unlock 的区间内，不得出现任何 `file_mgr_*()` / `sidecar_write_*()` / `fopen` / `fclose` / `fwrite` / `fflush` 调用（例外：兼容层与测试夹具显式白名单）。
2. 每个 `file_mgr_capture_*()` 的所有 return 分支都必须经过 `unlock(F)`；可用「函数内 lock 数 == unlock 数且每个出口可达」的静态检查或运行时断言。
3. `file_mgr_run_job()` 内不得出现 `pthread_mutex_lock(&g_ctx.mutex)` / `g_init_mutex` / `g_storage_mutex`。
4. 发布段的第一次 `lock(C)` 之前，本循环内 F 的引用计数必须为 0（由确定性用例在运行时断言，见 §13）。
5. **L5 关闭准入**：`dump_writer.c` 中 `destroy_session_locked()` 的非定义调用点必须**恰好两个**，且每个都被 `pending_blocks == 0` 条件门控（一个在 `stop_session_locked` 内，一个在发布段的 `SESSION_STOPPING` 分支内）。同时 `file_mgr_capture_open()` 在 `src/` 内的非定义调用点必须唯一（写线程 `execute_block()`），因为槽位淘汰只允许发生在写线程上（见 §8.1 第 6 条）。实现为 `test/check_lock_order.sh` 的判据 9。

---

## 13. 验收清单（可执行）

### 13.1 主判据：确定性并发用例（新增 2 例）

复用「确定性取证：在阶段二草图上证实或否证 F→C 边」的双栅栏 + `--wrap=pthread_mutex_lock/unlock` 锁归属审计原型（`dump_pcm_data/build/stage2/`，该目录被 `.gitignore` 忽略，**探针源码未入库**；构造、日志与复现命令见 [`docs/stage2-lock-order-evidence.md`](stage2-lock-order-evidence.md)，实施时按本清单把它改造成入库用例）：

- **反例（辨别力对照）**：人为在发布前保留 F，断言放行后 `ABBA == CLOSED`。若反例不成立，用例无效，验收失败。
- **正例（生产实现）**：断言放行后 `ABBA == no`，且「写线程取 C 时 F 已释放」（L2），且会话线程/写线程都完成。
- 判据是**锁归属表**，不是耗时；不依赖 OS 抖动。栅栏与超时必须用 `CLOCK_MONOTONIC` 真实时间（Windows 上 `usleep(100)` 近似 yield，固定循环次数会误判）；异常路径用 `_exit()` 绕过 atexit，否则 `shutdown_module → file_mgr_destroy` 会等 F 造成假死锁。

### 13.2 辅助判据：阻塞放大（探针）

复用 `build/probe/lock_block.c` 的结构，2000 blocks、连续 0 / 20 / 0 / 20 ms 四组，**PCM `fopen("ab")` 与 sidecar `fopen("wb")` 两个注入面分别跑**：

| 指标 | 0 ms 基线 | 20 ms 注入 | 说明 |
|---|---|---|---|
| `caller max` | <= 2 ms | <= 10 ms | 注入延迟不得线性传导到调用线程 |
| `blocked>=10ms` 计数 | 0 | 0 | 复用现有 `over_10ms` 指标 |
| `dropped_blocks` 组间差值 | — | <= 20（2000 的 1%） | 不得因锁竞争新增丢弃；用差值而非绝对值 |
| 反向对照 | — | 只注入 sidecar 20 ms、不停在发布前 | 进程正常退出、无 ABBA、sidecar 文件确实生成 |

### 13.3 回归门禁

- 67 + 2 = 69 用例全绿；`_Static_assert` 与提示字符串同步。
- Windows 原生全量套件 `sh test/run_full_windows.sh`（直连 gcc 与 `USE_CMAKE=1` 两条路径）、CTest、`mingw-compile` 门禁全绿。
- 负控门禁保持：把 `AUDIO_DUMP_DIR` 指向父为普通文件的路径，套件退出码非 0 且 `fails >= 40`（`.github/workflows/ci.yml:172-196`）。
- ASan / UBSan 无报告；`make` → `make run` → `make coverage` 顺序执行（AGENTS.md 强制验证规则；本机无 POSIX make，CI 为编译器记录）。
- 三份 wrap 列表一致；public header 无 POSIX-only include；MinGW 上 cond clock 回退 `CLOCK_REALTIME` 的用例仍绿。

### 13.4 必须逐项复核的行为不变

开关关闭零副作用（master switch 先于 init）、低存储闩锁（须 toggle 才清）、rollover 2000 ms gap、文件名格式、final sidecar 的 JSON 字段与「关开关仍 finalize」、legacy 无 format 不产 sidecar、finalize 数据留在 `file_info_t`、查询 API 未初始化返回 -1、`g_root_dir` 仅 init 前可改。

---

## 14. 逐文件改动清单

| 文件 | 改动 |
|---|---|
| `src/file_mgr_job.h`（新） | §5.1 的 POD 类型与 §5.2/5.3 原型 |
| `src/file_mgr.c` | 新增 capture/run 族；把 `finalize_slot_meta`(:216)、`close_slot_file`(:231)、`close_idle_file_if_needed`(:246) 改为产出 job；`file_mgr_get_handle`(:342)、`write`(:413)、`close`(:445)、`attach_meta`(:470)、`cleanup`(:534)、`init`(:310)、`destroy`(:324) 降级为兼容层 |
| `src/dump_writer.c` | `writer_thread_func`(:76) 改三段式；`close_session_file_locked`(:23)、`destroy_session_locked`(:30)、`stop_session_locked`(:42) 改成「C 内 capture / C 外 run」；移除 `:164` 的 fp 快照；`maybe_run_retention_cleanup`(:66) 保持锁外 |
| `src/dump_pcm_data.c` | `write_ex` 的 rollover stop(:266)、显式 `stop`(:287)、`shutdown_module`(:59) 的 I/O 迁移；入队与闩锁逻辑不动 |
| `src/session.c` / `session.h` | `session_file_action`(:189) 的 `handle_tracked` 改为接受 capture 结果或删除该参数；`session_info_t` 尽量不加字段 |
| `src/dump_internal.h` | 若新增跨 TU 的快照/作业声明则补原型；不新增锁 |
| `test/main.c` | 新增 2 个确定性用例；`_Static_assert` 计数与提示字符串同步；其余借助兼容层不动 |
| `Makefile` / `CMakeLists.txt` / `test/run_full_windows.sh` | 仅当新增 wrap 符号时同步三份列表 |
| `test/check_verification_flow.sh`（或新契约脚本） | §12 的静态判据 |

---

## 15. 明确不改动项

- `g_root_dir` 的三锁不一致（S/F 下读、I 下写，`file_mgr_set_root_dir` 自身零锁）：阶段二不重定归属，继续靠「仅 init 前可改」的约定规避。
- `dump_control.c` 的 `check_storage()`（C→S 边）与查询 API 的两段 C 区间。
- `sidecar.c` 的 JSON 字段、路径派生与原子替换方式。
- low-storage 闩锁、保留策略（100 文件 / 7 天）、长路径文件名裁剪。
- `pthread_condattr_setclock` 的回退逻辑、隔离层 `proc_iso.c`、负控门禁的夹具语义。

---

## 16. 交付边界

本规格是地图的终点：**决策已定稿，代码实施留给下游**。地图范围内不写生产代码、不改 CI、不发实施 PR；把本文迁入 `docs/` 属地图关闭**之后**的下游收尾动作，由独立文档 PR 完成，不改变规格的任何技术内容。

本仓库所有改动（含文档）须经 PR 合入 `main`，`main` 受 4 个必需检查（PC build/test/coverage、MinGW cross compile、Windows native full suite、Sanitizers）保护。**阶段二实施 PR 应把本文作为设计依据引用**，按 §12 的静态判据与 §13 的验收清单逐项验证；该 PR 是独立工作单元，不在本规格范围内。
