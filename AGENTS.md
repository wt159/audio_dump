# AGENTS.md

## Repo shape
- Work happens in `dump_pcm_data/`; the repo root only holds docs (`spec.md`, `design.md`, `plan.md`) and one module directory.
- The codebase is a single C11 audio HAL PCM dump module with PC test support and Android static-library integration.

## Source of truth
- Prefer executable config over prose. `dump_pcm_data/Makefile` and `dump_pcm_data/Android.mk` are authoritative.
- The root docs are useful context, but they are not always current. Verify behavior in code before updating docs.

## 强制验证规则
- 每次改动代码后，必须按顺序执行：编译 → 测试 → 覆盖率。
- 对这个仓库，PC 端验证顺序就是 `make` → `make run` → `make coverage`。

## Build and test
- PC build: `cd dump_pcm_data && make`
- PC smoke run: `cd dump_pcm_data && make run`
- Coverage smoke: `cd dump_pcm_data && make coverage` (this prints gcov output and still exits 0)
- Focused PC retry check: `AUDIO_DUMP_RUN_PTHREAD_CREATE_TEST=1 ./dump_pcm_test`
- Android build: `cd dump_pcm_data && mm`

## Runtime knobs
- PC test binary reads `AUDIO_DUMP_ENABLED` and optional `AUDIO_DUMP_DIR`.
- Android code uses the property `vendor.audio.dump`.
- `make run` creates `/tmp/audio_dump_test` and runs the test binary with dump enabled.

## High-signal implementation facts
- Public APIs are `dump_pcm_data_write()` and `dump_pcm_data_stop()`, both take string `key1/key2`; Android HAL calls them from `out_write()/in_read()` and `out_standby()/in_standby()`.
- The master switch is checked before initialization: a disabled `dump_pcm_data_write()` does not create the writer thread, does not create directories, and does not run the retention cleanup.
- Query APIs return -1 when the module has never been enabled, so "disabled" and "not initialized" are deliberately indistinguishable.
- `src/dump_pcm_data.c` is the runtime core: lazy singleton init, one writer thread, shared queue, session tracking, and file management.
- Current file names include escaped keys, stable hash, generation, split index, and timestamp in code.
- Low-storage handling latches until dump is toggled off and back on; do not assume space recovery alone clears it.
- `test/main.c` is a large PC test harness; if you change core logic, expect many assertions there.

## Keep in mind
- Generated artifacts live under `dump_pcm_data/` and are ignored via the local `.gitignore` (`*.o`, `*.gcda`, `*.gcno`, `*.gcov`, `dump_pcm_test`).
- Avoid editing docs to match stale prose unless the code behavior has also changed.
