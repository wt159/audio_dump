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

## Environment and CI constraints
- This workstation has no C toolchain: `gcc`, `make`, `cc` and `gcov` are absent, and `wsl.exe` is blocked by the sandbox policy. Never report a code change as locally verified; run the three steps on the CI `pc-verify` job or on a Linux host.
- `mingw-compile` is a required gate: it cross-compiles the module with mingw-w64 and must stay green. Keep the public header free of POSIX-only includes instead of widening `continue-on-error`.
- When `github.com:443` is unreachable (`CONNECT tunnel failed, response 502`, `Empty reply from server`, or a timeout) but `api.github.com` still answers, push by replaying commits through the Git Data API: `POST /git/trees` with `base_tree`, `POST /git/commits`, then `POST` or `PATCH /git/refs`, and open the PR with `gh pr create`.
- Replayed commits get new SHAs, so local and remote branches diverge even when the content matches. Reconcile with `git fetch && git reset --hard origin/main` once the network recovers; never push a diverged branch.
- When sending file content through the API, read it as bytes (`git show <sha>:<path>`), never through a trimming helper: stripping the trailing newline silently rewrites the file.
- Keep text files on LF. Python's text mode on Windows writes CRLF, which breaks the grep-anchored contract scripts (`check-verification-flow` fails with no obvious cause); use binary mode or the editor.
- Cross-platform onboarding for issue #1 is tracked in `docs/issue-1-cross-platform-plan.md`; keep its status table in sync with reality.

## Workflow
- Every change, documentation included, lands through a pull request. Do not commit to `main` directly.
- Keep PRs single-purpose: one behaviour change per PR, with the tests that cover it in the same commit series.

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
- Public APIs are `dump_pcm_data_write()`, `dump_pcm_data_write_ex()`, `dump_pcm_data_stop()`, `dump_pcm_set_enabled()` and `dump_pcm_set_root_dir()`. Android HAL uses `write`/`stop` from `out_write()/in_read()` and `out_standby()/in_standby()`; `write_ex` only adds a format argument, and NULL is exactly equivalent to `write`.
- The master switch is checked before initialization: a disabled `dump_pcm_data_write()` does not create the writer thread, does not create directories, and does not run the retention cleanup.
- Query APIs return -1 when the module has never been enabled, so "disabled" and "not initialized" are deliberately indistinguishable.
- The public header must stay free of `<sys/statvfs.h>` and `<pthread.h>`; test-only injection points live in `src/dump_test_hooks.h`. Free-space probing goes through `dump_storage_query_fn_t` (bytes out), `statvfs` on POSIX and `GetDiskFreeSpaceExA` on Windows.
- The retention cleanup (`file_mgr_cleanup(100, 7)`) runs once on the writer thread, not during init, so the caller thread never scans the directory.
- `src/dump_pcm_data.c` is the runtime core: lazy singleton init, one writer thread, shared queue, session tracking, and file management.
- Current file names include escaped keys, stable hash, generation, split index, and timestamp in code.
- Low-storage handling latches until dump is toggled off and back on; do not assume space recovery alone clears it.
- `test/main.c` is a large PC test harness; if you change core logic, expect many assertions there.

## Keep in mind
- Generated artifacts live under `dump_pcm_data/` and are ignored via the local `.gitignore` (`*.o`, `*.gcda`, `*.gcno`, `*.gcov`, `dump_pcm_test`).
- Avoid editing docs to match stale prose unless the code behavior has also changed.
