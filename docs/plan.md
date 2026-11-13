# PCM Dump 模块实施计划

## 阶段一：PC 端核心功能开发（已完成）

### 1.1 后台写线程完善
| 任务 | 描述 | 状态 |
|------|------|------|
| 1.1.1 | 后台写线程启动（已在 init 中添加 pthread_create） | ✅ |
| 1.1.2 | 后台写线程从队列消费数据并写入文件 | ✅ |
| 1.1.3 | 条件变量唤醒 + 轮询保底机制 | ✅ |
| 1.1.4 | 以 `dump_pcm_data_write()` 调用进入时间为准，补齐同 key 间隔 > 2 秒的隐式 stop + 新会话切分 | ✅ 已实现（`SESSION_ROLLOVER_GAP_MS = 2000`；用例 `test_write_gap_over_two_seconds_creates_new_file`、边界 `test_write_gap_exactly_two_seconds_keeps_same_file`） |
| 1.1.5 | 保证显式 `dump_pcm_data_stop()` 后，同 key 的下一次 `write` 一定开启新会话/新文件 | ✅ 已实现（用例 `test_stop_then_new_session_creates_new_file`） |

### 1.2 dump_pcm_data_stop() 完善
| 任务 | 描述 | 状态 |
|------|------|------|
| 1.2.1 | 标记会话为停止接收状态，并确保只影响 key1/key2 精确匹配的那条流 | ✅ |
| 1.2.2 | 等待队列中该会话数据处理完毕 | ✅ |
| 1.2.3 | 关闭文件句柄 | ✅ |
| 1.2.4 | 验证隐式 stop 和显式 stop 的语义一致，避免把 2 秒规则误实现成 5 秒会话超时 | ✅ 已实现：2 秒切分（`SESSION_ROLLOVER_GAP_MS`）与 5 秒句柄 LRU（`IDLE_TIMEOUT_SEC`）分离，由 1.1.4 / 1.1.5 的用例共同覆盖 |

### 1.3 文件管理完善
| 任务 | 描述 | 状态 |
|------|------|------|
| 1.3.1 | 文件分片逻辑（单文件 1GB 超过后新建） | ✅ |
| 1.3.2 | LRU 关闭（空闲超时 5 秒，仅用于文件句柄回收，不参与会话切分） | ✅ |
| 1.3.3 | 目录不存在时自动创建 | ✅ |

### 1.4 资源限制完善
| 任务 | 描述 | 状态 |
|------|------|------|
| 1.4.1 | 队列容量限制（800块/512MB） | ✅ |
| 1.4.2 | 最大并发会话数（16条） | ✅ |
| 1.4.3 | 最大文件句柄数（16个） | ✅ |

---

## 阶段二：PC 端测试验证（覆盖事实见 `docs/test-matrix.md`）

本阶段的逐项状态不再在这里维护——同一份事实写两处已经漂移过三次（PR #57 / #60 / #61）。
plan 项到用例的映射、断言级别与「确认不测」的理由，**唯一真相是 [`docs/test-matrix.md`](test-matrix.md)**，
由 `dump_pcm_data/test/check_test_matrix.sh` 在每次 `make run` / `make coverage` 时校验。

需要读本节时按这个顺序：`test-matrix.md` 看覆盖，`docs/test-gap-spec.md` 看判据与验收。

### 2.3 PC 验证顺序

1. `make`
2. `make run`
3. `make coverage`
4. `make coverage-ratchet`

`make coverage` 只打印 gcov 摘要、恒以 0 退出；棘轮是**独立的第四步**（`dump_pcm_data/test/check_coverage_ratchet.sh`，基线 `test/coverage_baseline.txt`，容差 0.5 pp，判据见 `docs/test-gap-spec.md` §6.2），CI 把这两步排在同一个 PC job 里。`make run` / `make coverage` 之前还会先跑 `make check-contracts`。

---

## 阶段三：Android 移植（待实机校准 2 秒切分语义）

### 3.1 环境适配
| 任务 | 描述 | 状态 |
|------|------|------|
| 3.1.1 | 目录路径从 /tmp 改为 /data/vendor/audio_dump/pcm/ | ✅ |
| 3.1.2 | 环境变量改为 Android property（vendor.audio.dump） | ✅ |
| 3.1.3 | 日志实现从 printf 改为 ALOG | ✅ |

### 3.2 Android 编译
| 任务 | 描述 | 状态 |
|------|------|------|
| 3.2.1 | 创建 Android.mk | ✅ |
| 3.2.2 | 链接 liblog 库 | ✅ |
| 3.2.3 | 编译为共享库 | ✅ |

### 3.3 Android 集成
| 任务 | 描述 | 状态 |
|------|------|------|
| 3.3.1 | 集成到 Audio HAL 模块 | ⏳ 待实机 |
| 3.3.2 | 在 out_write/in_read 中调用 `dump_pcm_data_write(buffer, buffer_size, key1, key2)`，其中 key1/key2 为字符串 | ⏳ 待实机 |
| 3.3.3 | 在 out_standby/in_standby 中调用 `dump_pcm_data_stop(key1, key2)` | ⏳ 待实机 |
| 3.3.4 | Android 侧验证同 key 的 2 秒内连续 write 保持单文件，> 2 秒时触发隐式 stop + 新会话 | ⏳ 待实机 |
| 3.3.5 | Android 侧验证 5 秒逻辑只影响文件句柄 LRU，不影响会话切分 | ⏳ 待实机 |
| 3.3.6 | Android 侧验证总开关 `vendor.audio.dump`、低存储锁存和 stop/write 交互 | ⏳ 待实机 |

---

## 阶段四：生产验证（待设备上确认）

### 4.1 实机测试
| 任务 | 描述 | 状态 |
|------|------|------|
| 4.1.1 | 验证 dump 功能正常，覆盖同 key ≤ 2 秒单文件、> 2 秒隐式 stop、显式 stop 后新会话 | ⏳ 待执行 |
| 4.1.2 | 验证 HAL 线程无阻塞 | ⏳ 待执行 |
| 4.1.3 | 验证存储空间检查生效，包含低存储锁存 | ⏳ 待执行 |
| 4.1.4 | 验证 5 秒超时只做文件句柄 LRU 回收，不会误切分会话 | ⏳ 待执行 |

### 4.2 性能测试
| 任务 | 描述 | 状态 |
|------|------|------|
| 4.2.1 | write() 延迟 < 1ms | ⏳ 待执行 |
| 4.2.2 | 内存占用 < 20MB | ⏳ 待执行 |
| 4.2.3 | 文件句柄 < 16 个 | ⏳ 待执行 |

---

## 完成进度

```
PC 端开发    ████████████████░░░░  80%
PC 端测试    见 docs/test-matrix.md（不再估算百分比）
Android 移植 ████████████░░░░░░░░  60%
实机验证     ░░░░░░░░░░░░░░░░░░░   0%
```

---

## 项目产出

- `spec.md` - 需求文档
- `design.md` - 技术方案
- `plan.md` - 实施计划
- `dump_pcm_data/` - 源码目录
  - `include/dump_pcm_data.h` - 公共头文件（含 DUMP_PCM_INTERNAL_DEBUG_API 宏门控）
  - `src/` - 源码（7个.c）+ 内部头文件（9个.h）
  - `test/main.c` - PC 测试程序
  - `test/whitebox.c` - 白盒覆盖测试
- `Android.mk` - Android 编译配置
  - `README.md` - 项目说明
