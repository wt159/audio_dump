# 确定性取证记录：阶段二草图的 `F→C` 边

- 日期：2026-11-12
- 来源：工单「确定性取证：在阶段二草图上证实或否证 F→C 边」([#52](https://github.com/wt159/audio_dump/issues/52)) 的结算评论；本文是入库副本。配套：[阶段二实施规格](stage2-lock-order-spec.md)、[ADR 0002 决策记录](adr/0002-stage2-lock-order-decisions.md)。
- 基准：`main = 50ab3f8`，取证期间工作区全程干净。探针源码与构建产物位于 `dump_pcm_data/build/stage2/`（被 `.gitignore` 的 `build/` 忽略，**未入库**）；**原始输出已归档**到 [`docs/stage2-lock-order-evidence/`](stage2-lock-order-evidence/)，含 [`log_A.txt`](stage2-lock-order-evidence/log_A.txt)、[`log_B.txt`](stage2-lock-order-evidence/log_B.txt)、[`log_C.txt`](stage2-lock-order-evidence/log_C.txt)、[`gdb_A.txt`](stage2-lock-order-evidence/gdb_A.txt)。归档时只做了两项净化：统一 CRLF → LF、去除行尾空白；内容与数值未改动。
- 工具：MinGW 7.3 `D:/Qt/Tools/mingw730_64/bin`，gdb attach（winpid 由 `tasklist //FO CSV` 交叉核对）。

## 构造

`build.sh` 把 `src/*.c` 原样编译，**只把 `file_mgr.c` 换成 `stage2_shim.c`**（`#include "../../src/file_mgr.c"` + 两个导出）。因此探针里的 C（`g_ctx.mutex`）和 F（`g_file_mgr_mutex`）都是生产代码里的真实静态对象，`dump_pcm_data_stop()` / `close_session_file_locked()` / `file_mgr_is_tracking()` / `file_mgr_close()` 全部是真实调用链。只把写线程的「决策 → I/O → 发布」换成阶段二最小形状：

1. 决策段取 C，不做 I/O；
2. 锁外 `fopen` 由 F 保护（`stage2_open_hold_F`，返回时 F **仍持有**）；
3. 重新取 C 发布 `session->fp`。

链接期插桩：`--wrap=fopen`（按 mode 分别注入 20 ms：`"ab"` = PCM 打开，`"wb"` = sidecar 临时文件）、`--wrap=pthread_mutex_lock/unlock`（每线程记录持有集合与等待目标）、`--wrap=file_mgr_is_tracking`（在 stop 持 C、尚未取 F 处设栅栏 P2）。写线程持 F 处设栅栏 P1。两个栅栏把双方钉在**各自只持一把锁、都还没去取对方锁**的状态，再同时放行 —— 判据是锁归属，不比较两次运行耗时。

变体：

| 变体 | 发布前是否释放 F | 栅栏 | 注入 | 预测 |
|---|---|---|---|---|
| A | 否（capture/write 未切干净） | 开 | PCM `fopen` 20 ms | ABBA 闭合 |
| B | 是（F 保持叶子锁） | 开 | PCM `fopen` 20 ms | 无环，正常完成 |
| C | 是 | 关（不停在发布前） | 仅 sidecar `fopen` 20 ms | 只变慢，不死锁 |

## 结果

### 变体 A（`log_A.txt` / `gdb_A.txt`）

```
[AT BARRIER]   session holds=C waits=-   writer holds=F waits=-   ABBA=no
[AFTER RELEASE] session holds=C waits=F   writer holds=F waits=C   ABBA=CLOSED
```

gdb attach（winpid 42996，log 内 `_getpid()` 与 `tasklist //FO CSV` 一致）：

```
Thread 6 (session): __wrap_pthread_mutex_lock(mutex=0x410060 <g_file_mgr_mutex>)
                    <- file_mgr_is_tracking (file_mgr.c:508)
                    <- close_session_file_locked (dump_writer.c:24)
                    <- destroy_session_locked (:35) <- stop_session_locked (:50)
                    <- dump_pcm_data_stop (dump_pcm_data.c:287)      [持 C，等 F]
Thread 5 (writer):  __wrap_pthread_mutex_lock(mutex=0x415040 <g_ctx>)
                    <- stage2_writer (probe.c:263, 发布点)             [持 F，等 C]
Thread 4:           writer_thread_func (dump_writer.c:102) cond timedwait，空闲
Thread 1:           探针主线程，停在 gdb 窗口
```

与「锁持有区间全量清单」的假说一致，也补齐了工单「写线程在锁内 fopen，会以 1.7 倍放大阻塞音频线程」里**缺失的会话线程栈帧归属**：那条链是 `dump_pcm_data_stop → stop_session_locked → destroy_session_locked → close_session_file_locked → file_mgr_is_tracking`，不是线程池等待。

### 变体 B（`log_B.txt`）

同一栅栏、同样 20 ms 注入，发布前 `stage2_drop_F()`：

```
[AT BARRIER]    session holds=C waits=-   writer holds=F waits=-   ABBA=no
[AFTER RELEASE] session holds=- waits=-   writer holds=- waits=-   ABBA=no
                session_done=1 writer_done=1
```

退出码 0，两条线程都完成。**同一草图、同样延迟，仅移动 F 的释放点就从死锁变成无环。**

### 变体 C（`log_C.txt`）

无栅栏、发布前释放 F、只对 sidecar `fopen("wb")` 注入 20 ms：

```
ABBA=no  session_done=1 writer_done=1 caller_done=1
caller max 61234 us, dropped 67, caller_done=1
```

生成 7 个 `.json` sidecar，证明注入确实生效；caller 明显变慢（61 ms 峰值），但进程正常退出、无死锁。**阻塞放大与 ABBA 是两种不同现象**，1.7 倍耗时本身不能当死锁证据。

## 一句可判定结论

**`F→C` 在发布点成立 —— 但成立条件是「写线程发布时仍持有 F」；一旦发布前释放 F，同一阶段二草图不产生 `F→C`。** 因此第 4 处耦合不是新的结构性位置，而是 **F 的释放点被推迟到发布之后**这一实现形状缺陷；工单「写线程在锁内 fopen」的 (1)(2)(3) 三层都只缩短了 I/O 的持锁时间，没有把 F 的释放点移到发布之前。

推论（供「阶段二锁序不变量与第 4 处耦合的修法选择」使用）：

- **F 必须保持叶子锁**：任何持 F 的路径不得再取 C / S / I。这条不变量足以消除环，无需拆分 C。
- #45 的两个候选建议都不必要：`is_tracking` 留在 C 内不会成环（B 变体里它原样存在）；`stop` 改 `STOPPING` + 写线程延迟销毁也不需要（B 变体里 stop 仍是同步销毁）。
- 修法收敛为接口契约：`file_mgr` 的 capture 必须在返回前释放 F，write 阶段不得持 F 访问任何 C 保护的数据；发布阶段单独取 C。
- B/A 两个栅栏变体可以直接改造成**确定性并发回归用例**：断言放行后的锁归属表，而不是断言耗时。

## 边界

- 这是 `50ab3f8` 上链接生产源码的**最小草图**取证，不是已回退的 #45 实现（那份代码不在仓库里）。结论是**条件性证实**：假说预测的形状确实闭合 ABBA，且 gdb 栈与 #45 的观察一致；不能证明 #45 的实现必然采用该形状。
- 探针关闭了 sidecar（A/B）以避免 final sidecar 成为第二个注入面；C 单独考察它。
- A/B 变体没有音频 caller 线程（交叉只发生在会话线程与写线程之间）；音频线程由变体 C 的 caller 覆盖，结果是无死锁、只变慢。
- 未改动任何生产源码，`git status` 干净，`main` 仍为 `50ab3f8`。

## 复现与查阅

**查阅（无需重建）**：`docs/stage2-lock-order-evidence/` 下的四份原始输出保留了完整的锁归属表、栈帧与注入读数；本文「结果」章节是它们的摘录。

**重建（需本地保留临时产物）**：探针源码（`probe.c`、`stage2_shim.c`、`build.sh`、`gdb.cmd`）是被忽略的临时取证代码，按地图 Out of scope **未入库**；实施阶段若要新建确定性回归用例，应按 [`stage2-lock-order-spec.md`](stage2-lock-order-spec.md) §13.1 重新实现并入库，而不是把这些临时文件直接搬进 `test/`。在本次取证的工作区中，复现命令为：

```sh
cd dump_pcm_data
sh build/stage2/build.sh
./build/stage2/abba_probe.exe B     # 无环
./build/stage2/abba_probe.exe C     # 只变慢
./build/stage2/abba_probe.exe A &   # 互锁 20 s
# 待日志出现 GDB_READY pid=...，与 tasklist //FO CSV 核对 winpid
gdb --batch -x build/stage2/gdb.cmd -p <winpid>
```
