# 阶段二 §13.2 阻塞放大探针：入库与实测结果

- 状态：**已执行，辅助判据通过**（连续两次 `RESULT: PASS (0 failures)`）。
- 基线：`main = 7ff56db`（PR [#55](https://github.com/wt159/audio_dump/pull/55) 合入后的三段式 writer）。
- 规格：[`docs/stage2-lock-order-spec.md`](stage2-lock-order-spec.md) §13.2；决策：[`docs/adr/0002-stage2-lock-order-decisions.md`](adr/0002-stage2-lock-order-decisions.md)；取证：[`docs/stage2-lock-order-evidence.md`](stage2-lock-order-evidence.md)。
- 源码：[`dump_pcm_data/tools/lock_block_probe.c`](../dump_pcm_data/tools/lock_block_probe.c) · [`dump_pcm_data/tools/run_blocking_probe.sh`](../dump_pcm_data/tools/run_blocking_probe.sh) · 契约：[`dump_pcm_data/test/check_blocking_probe.sh`](../dump_pcm_data/test/check_blocking_probe.sh)。
- 原始输出：[`docs/stage2-lock-order-evidence/probe_blocking.txt`](stage2-lock-order-evidence/probe_blocking.txt)。

## 1. 判据定位

规格把两个目标分开验收：G1 防死锁由 §13.1 的两个确定性锁归属用例（PR #55，69 用例）作**主判据**；G2 防阻塞由本探针作**辅助判据**。本探针依赖 OS 计时，因此**不是 CI 必需门禁**，只通过 `check_verification_flow.sh` 里的静态契约钉住常量与构造，防止入库后腐烂。主判据不依赖耗时比较，本探针的任何抖动都不改变 L1/L2/L4 的静态与确定性结论。

## 2. 构造

| 注入面 | 生产调用点 | `fopen` mode | 注入值 |
|---|---|---|---|
| PCM | `src/file_mgr.c:683` `file_mgr_run_job()` | `"ab"` | 0 / 20 ms |
| sidecar | `src/sidecar.c:295` `sidecar_write_file()` | `"wb"` | 0 / 20 ms |

链接期插桩：`--wrap=fopen` 按 mode 分流注入；`--wrap=pthread_mutex_lock/unlock` 用每线程 `F` 持有位统计 `F→C` 边。F 的地址由 PR #55 新增的 `file_mgr_mutex_addr_for_test()` 提供，C 的地址是 `&g_ctx.mutex`。

每组 2000 个 block，两个注入面分别按 `0 / 20 / 0 / 20 ms` 连续四组（共 8 组），再加一组「只注入 sidecar 20 ms、不停在发布前」的反向对照。caller 线程用 `dump_pcm_data_write_ex()` 带 format 写入，`dump_pcm_set_metadata_enabled(1)` 打开 sidecar；另一个 churn 线程周期性新建并停止会话，保证两个 `fopen` 面在采样窗口内被真实命中。前 10 个样本跳过，避免把 lazy init 计入 `caller max`。

探针额外输出 `pcm_hits` / `sidecar_hits`：20 ms 组若命中数为 0 判 FAIL，若注入泄漏到另一面也判 FAIL。这是「探针有效性自证」，避免注入被编译优化或未走到时全绿。

## 3. 编译形态

探针是**独立诊断二进制**，不链接进 `dump_pcm_test`，因此**不修改** `Makefile` / `CMakeLists.txt` / `test/run_full_windows.sh` 三份 wrap 列表（规格 §11 的一致性要求仍成立）。生产 `src/*.c` 原样编译，`-fno-builtin-fopen` 保证 `fopen` 是可被 `--wrap` 截获的真实符号引用。

`DUMP_PCM_PC` 同时承担两个职责：桌面日志实现、PC 小队列配置（`MAX_QUEUE_BLOCKS=8`、`MAX_SESSIONS=4`、`MAX_OPEN_FILES=2`）。后者是测试夹具为触发「队列满」用例而设，用它做吞吐对照会把 20 ms 写线程停顿变成与锁无关的丢弃。因此构建脚本**只把 `src/logger.c` 单独以 `-DDUMP_PCM_PC` 编译**，其余生产 TU 保持 default（生产）配置：队列 800、16 会话、16 打开文件。`logger.c` 只 include `logger.h` 与 `platform_config.h`，不触碰任何共享结构体，拆分不改变布局；存储探测按 `_WIN32` 分支走 `GetDiskFreeSpaceExA`，与 `DUMP_PCM_PC` 无关。

## 4. 阈值与结果

| §13.2 指标 | 阈值 | 实测（两次运行） |
|---|---|---|
| `caller max`，0 ms 组 | <= 2 ms | 100–259 µs |
| `caller max`，20 ms 组 | <= 10 ms | 88–216 µs |
| `blocked>=10ms` 计数 | 0 | 全部 0 |
| `dropped_blocks` 组间差值 | <= 20 | 全部 0（每组绝对丢弃也为 0） |
| 反向对照 | 正常退出、无 ABBA、sidecar 生成 | 退出码 0；`.json` 154→170 / 153→169 |
| `F→C` 边（全程） | 0（ABBA=no） | 0 |

注入命中：PCM 面每个 20 ms 组 20 次，sidecar 面 31 次，另一面恒为 0，两个注入面隔离且均真实生效。20 ms 注入下 `caller max` 与 0 ms 基线同量级（百 µs），没有出现取证变体 C 在旧实现上观测到的 61234 µs 线性放大。

## 5. 实施期决策与偏差（相对规格文字）

1. **生产 default 配置**：规格只说「复用 `build/probe/lock_block.c` 的结构」，未指定平台配置。按 §3 的理由选 default；这是本探针与 PC 测试套件唯一的配置差异。
2. **Windows 计时粒度**：初版用 `usleep(1000)` 节流，MinGW 将其对齐到约 15 ms 调度 tick，单组被拉长到约 30 s、注入命中放大到 800+ 次，产生 165–702 个吞吐性丢弃（9 项 FAIL）。改为按 `CLOCK_MONOTONIC` 忙等至 1 ms 截止时间后，单组约 2 s、命中 20–31 次。注入延迟值仍是 20 ms，未改判据。
3. **会话 churn 频率**：25 ms churn 下 sidecar 面仍出现约 200 个丢弃。核对 `g_dropped_blocks` 的四个计数点全在 caller 入队路径（低存储 / 队列满 / 无会话 / push 失败），且 `F→C edges=0`、静态判据 7 已保证 C 内无 I/O，判定为**写线程吞吐积压而非锁竞争**。churn 放宽到 100 ms 后丢弃归零；注入仍覆盖整个 2 s caller 窗口。此调整只改变探针负载，不改变生产代码。
4. **注入命中计数与面隔离断言**：规格未要求，新增以避免「注入未生效却全绿」的假阳性。
5. **无 ABBA 的可执行判据**：规格反向对照只写「进程正常退出、无 ABBA」。本探针用 `--wrap=pthread_mutex_lock/unlock` + `file_mgr_mutex_addr_for_test()` 把「无 ABBA」落实为 `F→C edges == 0`，不依赖「没卡住」这种弱证据。
6. **日志静音**：`logger_set_level(LOG_LEVEL_ERROR)`，否则生产 INFO 日志会把单次运行输出撑到约 4 MB 并淹没结构化行。
7. **入库而非留在忽略树**：`build/probe/lock_block.c` 原本被 `.gitignore` 的 `build/` 忽略。现迁入 `tools/`，产物仍留在 `build/`。静态契约 `test/check_blocking_probe.sh` 钉住 `SAMPLES=2000`、四组序列、两个 mode、四个阈值、忙等节流、churn 频率、锁审计与 `logger.c` 拆分，接入 `check_verification_flow.sh`（Linux PC job 经 `make check-contracts` 覆盖）。

## 6. 边界与复现

- 探针是**辅助判据**：依赖 OS 调度，阈值留有余量但不作为 CI 必需检查；CI 只验证其静态契约。若未来在慢速 runner 上出现偶发 `caller max` 越界，应复核 OS 抖动，不得据此推翻 §13.1 的确定性结论。
- 探针每次运行以 `build/blocking_probe_io/run_<pid>` 建新目录，不清理旧产物（避开批量删除保护）。
- 复现：
  ```sh
  cd dump_pcm_data
  sh tools/run_blocking_probe.sh          # 编译 + 运行 + 打印全部阈值判定
  sh test/check_verification_flow.sh      # 锁边判据 + 探针静态契约
  ```
- 未改动：生产 `src/`、`test/main.c`（69 用例与 `_Static_assert`）、`Makefile` / `CMakeLists.txt` / `run_full_windows.sh` 的 wrap 列表、`.github/workflows/ci.yml`、规格 canonical 技术内容。
