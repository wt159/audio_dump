# ADR 0002 — 阶段二锁层级重构：F 保持叶子锁，文件 I/O 移出 C

- Status: accepted
- Date: 2026-11-12
- Context: wayfinder 地图 [#48](https://github.com/wt159/audio_dump/issues/48)，工单 [#49](https://github.com/wt159/audio_dump/issues/49) / [#52](https://github.com/wt159/audio_dump/issues/52) / [#50](https://github.com/wt159/audio_dump/issues/50) / [#51](https://github.com/wt159/audio_dump/issues/51)
- 规格：[`docs/stage2-lock-order-spec.md`](../stage2-lock-order-spec.md)
- 证据：[`docs/stage2-lock-order-evidence.md`](../stage2-lock-order-evidence.md)

## Context

阶段一（PR #47，`main = 50ab3f8`）让 `file_mgr` 自带静态互斥锁 F，所有入口自持锁。此后 `dump_pcm_data_stop()`、写线程决策段、rollover 与 teardown 都在持有 C（`g_ctx.mutex`）的区间内调用 `file_mgr_*`，把 `fopen` / `fclose` / sidecar 的 open-write-rename 同时夹在 C 与 F 两把锁里。音频线程在 `dump_pcm_data_write_ex()` 入口等 C，因此写线程一次 20 ms 的 `fopen` 会把延迟放大到调用线程（工单 [#45](https://github.com/wt159/audio_dump/issues/45) 记录 1.7 倍）。

[#45](https://github.com/wt159/audio_dump/issues/45) 的三层重构把 I/O 挪到锁外，仍观察到死锁并整体回退，留下「第 4 处耦合」的猜测：`is_tracking` 或需要 `STOPPING` 延迟销毁。[工单 #49](https://github.com/wt159/audio_dump/issues/49) 的全量持有区间表证明 `50ab3f8` 的边集是 DAG（`C→F` 7 个源码点 / 9 条路径、`C→S` 1 个、孤立 F 5 个、**`F→C` = 0**），当前代码并无 ABBA 环。

[工单 #52](https://github.com/wt159/audio_dump/issues/52) 用链接生产源码的最小草图做确定性取证：双栅栏把会话线程钉在「持 C」、写线程钉在「持 F」，同时放行。变体 A（发布时仍持 F）→ `ABBA=CLOSED`；变体 B（仅在发布前释放 F）→ `ABBA=no`、两线程完成；变体 C（只给 final sidecar 注入 20 ms、不停在发布前）→ `caller max 61234 us`、`dropped 67`、无死锁。gdb 栈补齐了 #45 缺失的会话线程归属：`dump_pcm_data_stop → stop_session_locked → destroy_session_locked → close_session_file_locked → file_mgr_is_tracking`，持 C 等 F。

由此得到两条必须分开的结论：**死锁由「发布时仍持 F」引入的 `F→C` 造成；阻塞放大由「I/O 在 C 内」造成**。1.7 倍耗时本身不是死锁证据。

## Decision

1. **F 正式定为叶子锁（不变量 L1）**：任何持有 F 的路径不得获取 C / S / I，也不得调用可能获取它们的函数；每个 `file_mgr_*` 公开入口（含所有提前返回分支）必须在返回前释放 F。**不拆分 C**（L3），queue、session 表、`pending_blocks` 记账与会话销毁继续由单把 C 串行化。
2. **发布次序（L2）**：写线程一次循环中，F 的最后一个 unlock 必须严格早于第一次为发布而取 C。
3. **I/O 归属（L4）**：`fopen` / `fwrite` / `fflush` / `fclose` / sidecar open-write-rename / `opendir`-`stat`-`remove` 不得在持 C 或持 F 的区间内执行。
4. **修法路线**：采用「接口释放契约 + 写线程三段式（决策 / 执行 / 发布）」，把 capture（F 内纯内存、产出自包含 POD job、返回前释放 F）与 run（不持模块锁执行 I/O）拆成显式两族函数；发布单独取 C，用身份键 `(key1, key2, generation)` 重查会话，并以 `session->fp` 的**值**作次级身份比对（fp 值只作身份标签，绝不作写目标）。
5. **否决 #45 的两条候选为修法**：(a) 把 `file_mgr_is_tracking()` 移出 C、(b) `stop` 只标记 `STOPPING` + 写线程延迟销毁与周期扫描。取证变体 B 里两者都原样保留却不成环，说明它们都不是消除死锁的必要条件；但为达成 G2（防阻塞），所有 `file_mgr_*` 调用仍须迁出 C，状态机保持两态与同步销毁语义。
6. **保留薄兼容层**：旧 `file_mgr_*` 签名不变，供 `test/main.c` 约 90 处直调继续编译；生产路径一律走 capture/run，静态判据钉住这一点。
7. **规格粒度**：签名、身份键失效处理、五条失败路径、逐文件改动清单、可执行验收阈值全部写定（见规格 16 节）。验收以**确定性锁归属用例为主判据**（反例断言 ABBA 闭合、正例断言无 ABBA 且 F 已释放才取 C），20 ms 双注入面探针为辅助判据；判据不依赖耗时比较。

## Consequences

**代价**：改 6 个 `file_mgr_*` 入口的签名语义、重排 `dump_writer.c` 四个函数、`dump_pcm_data.c` 三处调用点、新增确定性用例；三份 wrap 列表与 `test/whitebox.c` 宏改名需保持一致。兼容层因此留下两条路径，规格注明「仅供测试夹具，禁止生产调用」，测试夹具为单线程，不构成生产边集。

**必须保持不变**：67 用例与 `_Static_assert`、Windows 全量套件与 CTest、负控门禁（`fails >= 40`）、开关关闭零副作用、低存储闩锁（须 toggle 才清）、rollover 2000 ms gap、文件名格式、final sidecar 的 JSON 字段与「关开关仍 finalize」、legacy 无 format 不产 sidecar、查询 API 未初始化返回 -1、`g_root_dir` 仅 init 前可改、MinGW 上 cond clock 回退 `CLOCK_REALTIME`、public header 不含 POSIX-only include、`mingw-compile` 门禁。`pending_blocks` 只在发布段 -1（作业失败也必须 -1）；`dropped_blocks` 语义不变且不得新增丢弃点。

**明确不做**：拆分 C、trylock / timedlock、延迟销毁状态机、多写线程 / 无锁化、重定 `g_root_dir` 的三锁归属、改 sidecar 内容格式。

**证据等级**：取证是 `50ab3f8` 上链接生产源码的**最小草图**，条件性证实，不是 #45 已回退的实现。规格因此用 L1/L2 的可断言判据与回归用例钉住结论，而不是依赖复现历史实现。

**边界**：本 ADR 与规格是地图的终点，**代码实施、发实施 PR、CI 变更均留给下游**；本仓库所有改动（含文档）须经 PR 合入 `main`。

## Addendum（2026-11-13）：L5 关闭准入，#45 的残余窗口不可达

实施完成后 [#45](https://github.com/wt159/audio_dump/issues/45) 仍被保留为 OPEN，理由是「PR #55 保留了一个跨 C 解锁窗口的 fp 生命周期竞态」。追查后该判断需要修正——**窗口不可达，不存在需要修的竞态**：

三段式把 `fwrite` 移到了不持 F 的 `run_jobs()`，理论风险是「WRITE job 已 capture、尚未 run 时，另一线程把同一 `FILE *` `fclose` 掉」。逐条枚举生产路径的关闭发起者（规格 §8.1 表 7 行）后可见，`src/dump_writer.c` 里 `destroy_session_locked()` 只有两个调用点，且都要求持 C 观察到 `session->pending_blocks == 0`；唯一另一类关闭是槽位淘汰，而 `capture_open()` 在 `src/` 内只被写线程 `execute_block()` 调用，与写操作同线程顺序执行。`pending_blocks` 在 C 内 `queue_push` 成功后 +1、只在发布段 C 内 −1，因此「决策段 → 发布段」之间恒 `>= 1`，与上述门控互斥。

由此新增 **L5（句柄关闭准入）**：`fclose` 只能由「持 C 且 `pending_blocks == 0`」或「对该 `FILE *` 独占所有权的线程」发起。这条不是新增约束，而是把实现里本就成立的准入条件显式化，并用静态判据（`test/check_lock_order.sh` 判据 9）钉住：`destroy_session_locked()` 的调用点必须恰好两个且都被门控，`capture_open()` 不得出现在写线程之外。

**对本 ADR 的影响**：Decision 第 5 条「否决延迟销毁状态机」得到加强而非动摇——所需状态早已存在（`pending_blocks` + 两态状态机），无需新增 STOPPED/DESTROYING 态或周期扫描。「明确不做」清单不变。#45 因此可以关闭，关闭依据是 §8.1 的可达性论证 + 判据 9，而不是「范围外、不修」。
