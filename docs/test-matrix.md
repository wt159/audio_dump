# 测试矩阵

**这是覆盖的唯一真相。** `docs/plan.md` 的第 2 章只指向本文件，不再自带状态列。
规格见 `docs/test-gap-spec.md`；分级判据见其 §3。本文件由 `test/check_test_matrix.sh` 校验：
引用的用例名必须存在于 `dump_pcm_data/test/main.c` 的 case 表，表中的每个用例也必须出现在这里。

字段：**用例** / **plan 项** / **级别** / **缺口类型** / **备注**。
`级别` 取值：`可失败`、`弱`、`恒真`、`编译期契约`、`确认不测`。`弱` 与 `恒真` 不允许存在（规格 §7）。

## 需求项覆盖

| 用例 | plan 项 | 级别 | 缺口类型 | 备注 |
|---|---|---|---|---|
| `test_string_key_write_and_escape` | 2.1.1 | 可失败 | — | 单文件内容与转义 |
| `test_key_validation` | 2.2.5 | 可失败 | — | NULL / 空 / 超长 |
| `test_argument_guard_coverage` | 2.2.5 | 可失败 | — | 公共 API 空参守卫 |
| `test_storage_cache_interval_hits_cached_path` | — | 可失败 | 断言缺口（已加固） | 断言缓存期内只查询一次存储 |
| `test_storage_cache_hits_cached_under_lock` | — | 可失败 | 断言缺口（已加固） | 两个写线程共享一次查询 |
| `test_session_separation_and_query` | 2.2.1 | 可失败 | — | 三会话互不合并 |
| `test_stop_then_new_session_creates_new_file` | 2.1.5 | 可失败 | — | 显式 stop 后新会话 |
| `test_write_gap_exactly_two_seconds_keeps_same_file` | 2.1.2 / 2.2.2 | 可失败 | — | 边界 2000 ms |
| `test_write_gap_over_two_seconds_creates_new_file` | 2.1.3 / 2.2.2 | 可失败 | — | 边界 2001 ms |
| `test_low_storage_latch_requires_reenable` | 2.1.8 | 可失败 | — | 锁存不因空间恢复解除 |
| `test_filename_hash_disambiguates_escaped_collisions` | 2.2.5 | 可失败 | — | 转义碰撞靠哈希区分 |
| `test_same_timestamp_rollovers_get_unique_files` | 2.2.3 | 可失败 | — | 同秒分片不重名 |
| `test_storage_failure_blocks_new_session` | 2.1.8 | 可失败 | — | 探测失败拦会话 |
| `test_storage_low_storage_blocks_new_session` | 2.1.8 | 可失败 | — | 低存储拦会话 |
| `test_storage_large_storage_reports_full_value` | 2.1.8 | 可失败 | — | 容量不被 32 位截断 |
| `test_queue_direct` | 2.1.7 | 可失败 | — | 队列容量与内容 |
| `test_queue_empty_pop_and_byte_limit` | 2.1.7 | 可失败 | — | 字节上限与空弹 |
| `test_session_direct` | 2.2.1 | 可失败 | — | 查找返回正确会话 |
| `test_session_highest_generation_lookup` | 2.2.1 | 可失败 | — | 取最高代 |
| `test_session_file_action_policy` | 2.2.1 | 可失败 | — | 重开 / 滚动策略 |
| `test_format_validation` | — | 可失败 | — | 格式校验 |
| `test_write_ex_captures_first_format` | — | 可失败 | — | 首个格式获胜 |
| `test_write_ex_without_format_matches_legacy` | — | 可失败 | — | write_ex 与 write 等价 |
| `test_sidecar_utf8_validation` | — | 可失败 | — | UTF-8 校验 |
| `test_sidecar_json_builder` | — | 可失败 | — | JSON 转义与字段 |
| `test_sidecar_write_meta_file` | — | 可失败 | — | 重写覆盖旧 JSON |
| `test_sidecar_switch_state` | — | 可失败 | — | 元数据开关 |
| `test_sidecar_write_file_failure` | — | 可失败 | — | 不可写目录报错 |
| `test_sidecar_lifecycle_on_real_file` | 2.1.1 | 可失败 | — | total_bytes 与文件一致 |
| `test_sidecar_legacy_write_leaves_no_sidecar` | — | 可失败 | — | legacy 路径无 sidecar |
| `test_sidecar_disabled_metadata_leaves_no_sidecar` | — | 可失败 | — | 关元数据无 sidecar |
| `test_sidecar_split_creates_two_sidecars` | 2.2.3 | 可失败 | — | 每片一个 sidecar |
| `test_sidecar_rollover_creates_new_sidecar` | 2.1.3 | 可失败 | — | 滚动新 sidecar |
| `test_sidecar_survives_without_stop` | — | 可失败 | — | 未 stop 不标 complete |
| `test_queue_allocation_failures` | 2.1.7 | 可失败 | — | malloc 失败不入队 |
| `test_session_allocation_failures` | — | 可失败 | — | calloc 失败不建会话 |
| `test_public_write_allocation_failures` | — | 可失败 | — | 分配失败不留会话 |
| `test_writer_queue_pop_failure` | 2.1.7 | 可失败 | — | pop 失败被记录 |
| `test_writer_queue_full_write_rejection` | 2.1.7 | 可失败 | — | 满队拒收 |
| `test_writer_queue_full_failed_enqueue_keeps_rollover_clock` | 2.1.4 | 可失败 | — | 入队失败不重置切分时钟 |
| `test_session_capacity_rejection` | 2.2.4 | 可失败 | — | 会话数上限 |
| `test_public_write_rejects_when_session_capacity_reached` | 2.2.4 | 可失败 | — | 公共写拒绝越界会话 |
| `test_stop_while_pending_blocks` | — | 可失败 | 断言缺口（已加固） | 断言待写块确实落盘 |
| `test_session_helper_direct` | 2.2.1 | 可失败 | — | 重复创建不重复登记 |
| `test_file_manager_direct` | — | 可失败 | — | 短写 / flush / 长路径 |
| `test_file_manager_reuses_tracked_handle` | 2.2.4 | 可失败 | — | 同路径不重复 fopen |
| `test_file_manager_closes_fp_when_no_free_slot` | 2.2.4 | 可失败 | 断言缺口（已加固） | 断言尾巴分支没留下句柄 |
| `test_file_manager_idle_scan_skips_inactive_slots` | 2.2.4 | 可失败 | 断言缺口（已加固） | 断言被驱逐的是在用槽 |
| `test_file_manager_rejects_when_full_and_no_idle_slot` | 2.2.4 | 可失败 | — | 满表且无空闲槽则拒绝 |
| `test_file_manager_close_rejects_foreign_handle` | — | 可失败 | — | 外来句柄不被关闭 |
| `test_file_manager_cleanup_filters_non_pcm_files` | — | 可失败 | — | 清理不误删 |
| `test_get_active_sessions_argument_guards` | 2.2.5 | 可失败 | — | 查询参数守卫 |
| `test_file_manager_cleanup_and_evict` | 2.2.4 | 可失败 | 断言缺口（已加固） | 断言驱逐的是最旧槽 |
| `test_file_manager_init_and_open_failures` | — | 可失败 | — | init / open 失败路径 |
| `test_file_manager_init_creates_and_destroy_closes` | — | 可失败 | 断言缺口（已加固） | destroy 后文件可删除 |
| `test_file_manager_cleanup_realloc_failure` | — | 可失败 | — | realloc 失败被拒 |
| `test_logger_direct` | — | 可失败 | — | 级别过滤 |
| `test_init_retry_path` | — | 可失败 | — | 重试退避 |
| `test_init_failure_path` | — | 可失败 | — | 坏路径 init 报错 |
| `test_init_do_init_failures` | — | 可失败 | — | 失败点不被吞 |
| `test_init_setclock_fallback` | — | 可失败 | — | CLOCK_REALTIME 回退 |
| `test_disabled_write_has_no_side_effects` | — | 可失败 | — | 总开关关闭无副作用 |
| `test_cleanup_deferred_to_writer_thread` | — | 可失败 | — | 清理不在调用线程 |
| `test_runtime_controls` | — | 可失败 | — | 开关 / 根目录 |
| `test_init_wait_path` | — | 可失败 | 断言缺口（已加固） | 断言等待期数据不丢；两个 block 的落盘顺序不参与断言（探针线程与触发写入竞争队列，顺序由调度器决定） |
| `test_whitebox_coverage_shim` | — | 可失败 | 断言缺口（已加固） | 断言守卫后状态可查 |
| `test_retention_reapplies_within_one_process` | — | 可失败 | — | 同进程内再次清理 |
| `test_lock_order_negative_control` | — | 可失败 | — | 反例必须被探针抓到 |
| `test_lock_order_production_path` | — | 可失败 | — | 生产路径零 F→C 边 |
| `test_file_split_via_public_api` | 2.2.3 | 可失败 | 需求缺口（已补） | 注入限值，公共 API 跨片，逐字节计数 |
| `test_idle_timeout_keeps_session_and_file` | 2.2.6 | 可失败 | 需求缺口（已补） | 回填空闲戳，会话存活且复用同一文件 |
| `test_concurrent_write_no_loss` | 2.1.6 | 可失败 | 需求缺口（已补） | 冻结写线程，两线程不丢不重 |

## 编译期契约

| 用例 | plan 项 | 级别 | 缺口类型 | 备注 |
|---|---|---|---|---|
| `config_contract.c` | — | 编译期契约 | 需求缺口（已补） | 不带 `-DDUMP_PCM_PC` 单独编译，钉住 16 / 16 / 5 / 800 / 512MB / 1GB |

## 确认不测

| 用例 | plan 项 | 级别 | 缺口类型 | 备注 |
|---|---|---|---|---|
| `并发 stop × write 交错` | 2.1.6 | 确认不测 | 需求缺口 | 需要新 seam 才能把 `stop` 钉在 `write()` 内部；现有栅栏只覆盖写线程启动前（见 #64） |
| `句柄收支配对` | 2.2.4 | 确认不测 | 需求缺口 | 需要 `--wrap=fopen/fclose` 并同步三份 wrap 列表；`mgr.count` 不足以发现泄漏（见 #64） |
| `Android 实机路径` | 3.3.4–3.3.6 / 4.1 | 确认不测 | — | 需要设备，属另一域 |
| `性能目标 <1ms / RSS / fd` | 4.2 | 确认不测 | — | PC 夹具上量延迟必然 flaky；延迟证据由 `tools/lock_block_probe.sh` 提供 |
