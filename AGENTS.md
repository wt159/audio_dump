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
- This workstation has no `gcc`/`make` on PATH and `wsl.exe` is blocked, but Qt 5.14.2 ships a MinGW-w64 7.3 toolchain at `D:/Qt/Tools/mingw730_64/bin`. It runs both the Windows smoke (`sh test/run_windows_smoke.sh`) and the complete 66-case suite (`sh test/run_full_windows.sh`); the harness no longer uses `fork`/`exec`, so nothing forces the suite onto Linux. `make` / `make run` / `make coverage` still need CI or a Linux host.
- Never report a code change as locally verified unless the Windows smoke or CI actually ran.
- `mingw-compile` is a required gate: it compiles every `src/*.c` for Windows on a bare command line (no harness, no `--wrap`, no link) and must stay green. It runs on `windows-latest` because apt-installing a cross toolchain onto `ubuntu-latest` stalled for over half an hour on three consecutive runners. Keep the public header free of POSIX-only includes instead of widening `continue-on-error`.
- The shell's `HTTPS_PROXY` points at `127.0.0.1:64082`, which answers `502 Bad Gateway` for `github.com:443` while `api.github.com` still works. The real client listens on `127.0.0.1:7892` (Clash mixed port), and this repository sets `http.proxy` to it. Before falling back to the API replay trick, test: `curl -s -o /dev/null -w '%{http_code}\n' --proxy http://127.0.0.1:7892 https://github.com` (expect 200).
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
- Windows full suite: `cd dump_pcm_data && sh test/run_full_windows.sh` (66 cases, native MinGW; `MINGW_BIN` overrides the toolchain, `USE_CMAKE=1` builds through CMake instead of a direct `gcc` line)
- Windows suite through CTest: `cmake -S . -B build/win -G "MinGW Makefiles" -DDUMP_PCM_BUILD_TESTS=ON && cmake --build build/win && ctest --test-dir build/win --output-on-failure`
- Focused PC retry check: `AUDIO_DUMP_RUN_PTHREAD_CREATE_TEST=1 ./dump_pcm_test`
- Android build: `cd dump_pcm_data && mm`

## Runtime knobs
- PC test binary reads `AUDIO_DUMP_ENABLED` and optional `AUDIO_DUMP_DIR`.
- Android code uses the property `vendor.audio.dump`.
- `make run` creates `/tmp/audio_dump_test` and runs the test binary with dump enabled.

## High-signal implementation facts
- Public APIs are `dump_pcm_data_write()`, `dump_pcm_data_write_ex()`, `dump_pcm_data_stop()`, `dump_pcm_set_enabled()`, `dump_pcm_set_root_dir()` and `dump_pcm_set_metadata_enabled()`. Android HAL uses `write`/`stop` from `out_write()/in_read()` and `out_standby()/in_standby()`; `write_ex` only adds a format argument, and NULL is exactly equivalent to `write`.
- The master switch is checked before initialization: a disabled `dump_pcm_data_write()` does not create the writer thread, does not create directories, and does not run the retention cleanup.
- Query APIs return -1 when the module has never been enabled, so "disabled" and "not initialized" are deliberately indistinguishable.
- The public header must stay free of `<sys/statvfs.h>` and `<pthread.h>`; test-only injection points live in `src/dump_test_hooks.h`. Free-space probing goes through `dump_storage_query_fn_t` (bytes out), `statvfs` on POSIX and `GetDiskFreeSpaceExA` on Windows.
- The retention cleanup (`file_mgr_cleanup(100, 7)`) runs once on the writer thread, not during init, so the caller thread never scans the directory.
- `src/dump_pcm_data.c` is the runtime core: lazy singleton init, one writer thread, shared queue, session tracking, and file management.
- Current file names include escaped keys, stable hash, generation, split index, and timestamp in code.
- Low-storage handling latches until dump is toggled off and back on; do not assume space recovery alone clears it.
- On MinGW 7.3 `pthread_condattr_setclock` rejects `CLOCK_MONOTONIC`, so the writer falls back to `CLOCK_REALTIME`; the Windows smoke asserts that path runs without failing.
- `test/main.c` is a large PC test harness; if you change core logic, expect many assertions there. `test/proc_iso.c` is the only place that spawns a child process, and `test/test_plat.h` the only place with platform spellings (`mkdir`, `setenv`, the test root). A `_Static_assert` pins the case count at 66.
- `-Wl,--wrap=` does work on MinGW PE, but three things break it silently: defining `DLL_EXPORT` (turns the winpthread declarations into `dllimport`), wrapping a call that is already resolved inside the same translation unit, and linking with `-static` (that also intercepts winpthreads' internal calls). The test target must never define `DLL_EXPORT`.
- Isolated cases re-execute the test binary through `iso_run()`, which passes an explicit environment block rather than mutating the parent's. A spawn that cannot be waited on is reported as a failure; treating "no result" as success is how the old `fork()` code once passed a failing case.
- The harness refuses to report PASS on a broken fixture: `reset_test_dir()` latches a `g_fixture_error`, `pass()` downgrades to FAIL, and the file-counting helpers return -1 instead of 0 when the directory cannot be read. Verify the latch still works by pointing `AUDIO_DUMP_DIR` at a path whose parent is a regular file — most of the suite must go red.

## Keep in mind
- Generated artifacts live under `dump_pcm_data/` and are ignored via the local `.gitignore` (`*.o`, `*.gcda`, `*.gcno`, `*.gcov`, `dump_pcm_test`).
- Avoid editing docs to match stale prose unless the code behavior has also changed.

## Agent skills

### Issue tracker

Issues live in GitHub Issues; use `gh` for every operation. See `docs/agents/issue-tracker.md`.

### Triage labels

Five canonical roles mapped to their default strings (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` at the repo root and ADRs under `docs/adr/`. See `docs/agents/domain.md`.
