# audio_dump

Android Audio HAL 的 PCM 抓取落盘模块（C11），支持 PC 端独立编译测试。

## 构建与测试

```bash
cd dump_pcm_data
make                   # PC 构建
make run               # 冒烟运行
make coverage          # 覆盖率
make coverage-ratchet  # 覆盖率棘轮（低于基线即非 0 退出）
```

Android 集成构建：`cd dump_pcm_data && mm`

## 目录

- `dump_pcm_data/` — 模块源码、PC 测试与构建脚本，详见 [dump_pcm_data/README.md](dump_pcm_data/README.md)
- `docs/` — 规格、设计与实施计划；测试覆盖矩阵 [docs/test-matrix.md](docs/test-matrix.md) 是「用例 → plan 项 → 断言级别 → 缺口类型」的唯一真相，判据与验收见 [docs/test-gap-spec.md](docs/test-gap-spec.md)；阶段二锁序重构：[实施规格](docs/stage2-lock-order-spec.md) · [取证记录](docs/stage2-lock-order-evidence.md) · [阻塞放大探针结果](docs/stage2-lock-order-probe.md) · [ADR 0002](docs/adr/0002-stage2-lock-order-decisions.md)
- `AGENTS.md` — 项目约定与验证流程
