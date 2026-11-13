# 测试套件缺口收口规格

> 目的：把 PC 测试套件从「名义覆盖」变成「可失败的断言」。
> 来源：地图 [#62](https://github.com/wt159/audio_dump/issues/62) 及其子工单 #63–#75。
> 实施：本规格落地后按「实施切片」逐片开单用途 PR；本文件只描述判据，不描述进度。

## 1. 范围

**在范围内**：`dump_pcm_data/test/` 的 PC 套件、`test/check_*.sh` 契约、`tools/` 探针以外的全部测试资产、`docs/test-matrix.md`、`.github/workflows/ci.yml` 的测试相关阈值。

**不在范围内**：

- `docs/plan.md` 阶段三/四的 Android 实机用例（3.3.4–3.3.6、4.1）——需要设备。
- `docs/plan.md` 4.2 的性能目标（`write()` <1ms、RSS <20MB、fd <16）——PC 夹具上量延迟必然 flaky，延迟证据由 `tools/lock_block_probe.sh` 提供。
- 生产**行为**改动。允许扩展测试专用注入点，但必须落在 `src/dump_test_hooks.h` 的声明加 `src/*.c` 内 `DUMP_PCM_PC` / `!ANDROID` 守卫的实现（先例 `g_dump_fwrite_fn`，`src/file_mgr.c:34,958`），不得新增生产行为分支。

## 2. 三类缺口的定义

| 术语 | 定义 | 判据 |
|---|---|---|
| 需求缺口 | `docs/plan.md` 承诺的行为没有对应用例 | 矩阵中该 plan 项的「用例名」列为空且「确认不测理由」列为空 |
| 断言缺口 | 有用例，但断言在被测机制坏掉时不会变红 | 三档分级（见 §3）中不为「可失败」 |
| 文档漂移 | 状态列、索引或进度数字与树不符 | 文档契约脚本或矩阵校验红 |

三者必须分开计数。把「未测试」与「测试无效」混为一谈，会让补用例的动作掩盖真实风险：69 条用例里已有 7 条不会因缺陷变红（#63）。

## 3. 断言分级

| 级别 | 定义 | 处置 |
|---|---|---|
| 可失败 | 断言要求被测机制产出确定值（字节内容、文件数、计数、特定非零/哨兵返回），存在一种缺陷能把它取反 | 保留 |
| 弱 | 只区分「是否正常返回」，机制做错但仍正常返回时为绿 | 必须加固为可失败 |
| 恒真 | 机制完全不执行时断言也成立 | 必须改写或删除 |

分级判据必须可由第三方仅读断言行复现。一条用例的每一轮加固都要附一次**负控**：临时植入目标缺陷 → 该用例 FAIL → 还原。负控证据以四列表格记在 PR 正文（用例名 / 植入的缺陷 / 失败输出首行 / 还原方式），不入库证据文件。

## 4. 需求缺口与判据

### 4.1 plan 2.2.3 文件分片（>1GB 自动分片）

现状：零覆盖。只有 1 字节 / 10 字节注入限值的间接路径，未走公共 API 验证「分片后内容完整」。

判据（`test_file_split_via_public_api`）：

1. 走公共 API：`dump_pcm_set_file_size_limit_for_test(4096)`，三次 `dump_pcm_data_write` 后 `dump_pcm_data_stop`。
2. 分片数 = 期望值，路径互不相同。
3. 各分片文件名中的 split 段递增（文件名含 split index；由 `generate_filename` 生成）。
4. 所有分片按 split 顺序拼接后的字节等于总载荷——不只是字节总数相等。
5. 每片 sidecar 的 `total_bytes` 等于该片字节数（写侧 `complete` 在 stop 后才置位）。

1GB 常量本身不由行为用例证明，归 §5 的常量契约。

### 4.2 plan 2.2.6 5 秒超时只归还句柄

现状：`close_idle_file_if_needed()`（`src/file_mgr.c:256-298`）只在槽表满时被调用；两处用例靠 `sleep(IDLE_TIMEOUT_SEC+1)` 真实等待，且只断言 `mgr.count`，**从未断言会话存活**。这才是真实缺口——不是「没跑过驱逐」。

判据（`test_idle_timeout_keeps_session_and_file`）：

1. 不 sleep：打开 A、B 两个会话占满槽表后，把两槽的 `last_write_time` 回填为 0，使 `now - last` 恒超阈值。
2. 写第三个会话 C → 断言恰好驱逐一个槽（`mgr.count` 回到 `MAX_OPEN_FILES`）。
3. 断言被驱逐的是**最旧**的槽（A），不是任意槽。
4. 断言 A 的会话仍存活（`active_session_count` 不变）且再次写 A 落回**同一路径**，字节累加。
5. 生产值 5 秒与 PC 值 1 秒的差异由 §5 常量契约承担，不由本用例假装证明。

回填 `last_write_time` 是越层写内部状态，必须由 `test/check_test_matrix.sh` 之外的一条注释解释其必要性；这是本规格唯一允许的越层断言。

### 4.3 plan 2.1.6 多线程并发 write

现状：唯一一条（`test_storage_cache_hits_cached_under_lock`）两线程同 key 同时写，断言只有 `active_session_count == 0`，无法发现丢块、重复或内容交错。

判据（`test_concurrent_write_no_loss`），只用既有 seam、不新增 wrap：

1. 用 `fake_pthread_create_hold_writer` 把写线程冻结在 `writer_thread_func` 之前（此时队列不被消费、磁盘为空）。
2. 两个线程经 barrier 同时 `dump_pcm_data_write`，各 4 块、每块 64 字节，载荷分别为 `'A'` 与 `'B'` —— 共 8 块，正好等于 PC 队列容量 `MAX_QUEUE_BLOCKS`。
3. 冻结期间断言 `queue_pending_blocks == 8` 且磁盘无 `.pcm`（证明「入队成功」与「尚未落盘」同时成立）。
4. 释放写线程并 `stop` 后断言：文件数 1、总字节 512、`'A'` 与 `'B'` 各 256 —— 不假设块序。
5. 断言 `dropped_blocks` 增量为 0。

**明确不做**：并发 `stop` × `write`、句柄收支配对。二者需要新 seam（`--wrap=fopen/fclose` 或跨线程写标记），已在 #64 论证；在矩阵里记「确认不测 + 理由」，不作为本规格的承诺。

## 5. 生产限值常量契约

测试构建全部带 `-DDUMP_PCM_PC`，生效的是 `src/platform_config_pc.h` 的 4 / 2 / 1 / 8 / 64KB；生产 `src/platform_config_default.h` 的 16 / 16 / 5 / 800 / 512MB 与 `FILE_SIZE_LIMIT` 1GB（`src/platform_config.h:18`）**在整套件里从未被实例化**，改名改值不会有任何用例变红。

判据（`test/config_contract.c`，独立 TU，无符号）：

1. `#include "platform_config_default.h"` 与 `"platform_config.h"`，用 `_Static_assert` 把六个常量钉在规格值：`MAX_SESSIONS 16`、`MAX_OPEN_FILES 16`、`IDLE_TIMEOUT_SEC 5`、`MAX_QUEUE_BLOCKS 800`、`MAX_QUEUE_BYTES 512MB`、`FILE_SIZE_LIMIT 1GB`。
2. 编译即断言，不需要运行；因此它随 `make`、CMake、`run_full_windows.sh` 三条链路的**编译**生效。
3. **不进** `Android.mk`：Android 用同一个 default 配置，但该 TU 属于 PC 套件。
4. 它的失败形态是编译错误，不是用例 FAIL——矩阵里以「编译期契约」标注，不计入用例数。

## 6. 事实载体

### 6.1 `docs/test-matrix.md`

唯一真相。每行一个用例，字段：`用例名 | plan 项 | 级别 | 缺口类型 | 备注`。「级别」与「缺口类型」分列，不用枚举混写。

`test/check_test_matrix.sh`（由 `check_verification_flow.sh` 转调）校验：

- 矩阵引用的用例名必须存在于 `test/main.c` 的 case 表（按 `{"name", fn}` 形状抽取）。
- case 表中的每个用例必须出现在矩阵里（缺失即红，不是警告）。
- 级别只允许 `可失败` / `弱` / `恒真` / `编译期契约`。
- 「确认不测」行的理由不得为空。

### 6.2 覆盖率棘轮

`test/coverage_baseline.txt` 存「文件 → 千分比」，**不存 `.gcov` 正文**（正文含 `Runs/Graph/Data`，每次运行都变）。

- 提取：单遍 `awk`，用「上一行是 `File '<path>'`」做守卫——`gcov -f` 同时输出每函数同名格式行，无守卫会多抓 8 倍。
- 只对 8 个 `.c` 定基线，排除头文件（`key_utils.h` 的分母随请求的 TU 集合在 8 / 36 之间变）。
- 容差：单文件下降超过 0.5 个百分点即红；合计下降超过 0.5 即红。零容差不成立——分母随 `-O` 级别与 gcc 版本漂（同一文件 `-O0` 为 `82.14% of 84`，`-O2` 为 `86.49% of 74`），多线程套件的命中行集合本身不确定。
- 基线绑定 CI 镜像与 gcc 版本：换镜像或换编译器视为**重基线事件**，不是回归。
- 逃生阀：`COVERAGE_RATCHET=0` 跳过硬门禁（仅本地）。
- `make coverage` 的退出码语义变更，必须同时更新 `test/check_verification_flow.sh` 钉住的目标图。

### 6.3 CI 负控阈值

`ci.yml` 在坏根目录下要求 `fails >= 40`，该数字按 66 用例标定（`test/main.c:41-49` 注释写「46 of 66 fail and 20 still pass」），套件现为 69+ 用例。判据：在**当前用例数**下实测 fail 数，把阈值钉成实测值，并在注释写明标定依据与「新增用例需重标」。

## 7. 验收

本规格视为落地，当且仅当：

1. `docs/test-matrix.md` 存在且 `check_test_matrix.sh` 在 `make check-contracts` 中通过。
2. 矩阵中不再有「弱」或「恒真」级别——两者都已加固或被明确标注为「确认不测」并附理由。
3. §4 的三组用例与 §5 的契约 TU 已入库，并各自附过一次负控。
4. 四个必需检查全绿：`PC build, test and coverage`、`MinGW cross compile check`、`Sanitizers (ASan + UBSan)`、`Windows native full suite`。
5. 覆盖率棘轮在 PC job 上生效（首次以 bootstrap 写入基线的那一次绿不算生效）。

## 8. 实施切片

一片一 PR，单片可独立验证：

1. **契约与矩阵**：`config_contract.c` + 三处构建接线 + `docs/test-matrix.md` + `check_test_matrix.sh` + `plan.md` 2.x 改为指针。无行为改动。
2. **断言加固**：7 条弱/恒真用例逐条加固，附负控表。只动 `test/main.c` 与 `test/whitebox.c`。
3. **新用例**：§4.1 分片、§4.2 超时、§4.3 并发，`_Static_assert` 用例数同步。
4. **棘轮与负控阈值**：`check_coverage_ratchet.sh` + 基线 + CI 接线 + `ci.yml` 阈值与 `main.c:41-49` 注释重标。
