# Windows 全量测试套件

- 关联：地图 #25；ADR `docs/adr/0001-test-process-isolation.md`
- 状态：已实现，本机 MinGW 7.3 与 CTest 两条路径均全绿通过
- 日期：2026-11-11

## 目标与结果

同一份用例源码，在 Windows 上原生跑完并通过，用例名与数量不变、没有 skip、没有 `#ifdef` 分支断言。`fork` / `exec` / `waitpid` 收敛到一个显式的进程隔离层。

## 新增与改动的文件

| 文件 | 作用 |
|---|---|
| `test/proc_iso.h` / `test/proc_iso.c` | 进程隔离层。`iso_run()` 以显式环境块重新执行本二进制并取回退出码；POSIX 走 `fork` + `execve` + `waitpid`，Windows 走 `CreateProcessA` + `WaitForSingleObject` + `GetExitCodeProcess` |
| `test/test_plat.h` | 平台拼写差异：`tp_mkdir()`、`tp_setenv()` / `tp_unsetenv()`、`tp_test_root()`。头文件内 `static inline`，不引入额外目标文件 |
| `test/main.c` | 15 处 `fork` → `iso_run()`；18 处 `/tmp/audio_dump_test` → `tp_test_root()`；4 处双参 `mkdir` → `tp_mkdir()`；`/dev/null/subdir` 与 `/proc` 夹具改为可移植命题；用例改为注册表并由 `_Static_assert` 钉住数量 |
| `test/run_full_windows.sh` | 本机与 CI 的 Windows 入口，直接 `gcc` 或 `USE_CMAKE=1` |
| `CMakeLists.txt` | `DUMP_PCM_BUILD_TESTS` 选项，注册 `add_test(pc-suite)` |
| `Makefile` | `TEST_OBJ` 加入 `proc_iso.o`（Linux 侧唯一改动） |
| `.github/workflows/ci.yml` | 新增 `windows-full-suite` job；`pc-verify` 的 CMake 步骤改为同时构建测试目标并跑 `ctest` |

## 怎么跑

```sh
# 本机（默认 Qt 自带工具链）
cd dump_pcm_data && sh test/run_full_windows.sh

# 指定工具链
MINGW_BIN=/c/mingw64/bin sh test/run_full_windows.sh

# 走 CMake 构建，再由 CTest 执行
cmake -S . -B build/win -G "MinGW Makefiles" -DDUMP_PCM_BUILD_TESTS=ON
cmake --build build/win && ctest --test-dir build/win --output-on-failure
```

Linux 侧仍是 `make` → `make run` → `make coverage`，行为不变，只是链接时多一个 `proc_iso.o`。

## 三处夹具改写，各自换了命题

| 原夹具 | 问题 | 改后 |
|---|---|---|
| `/dev/null/subdir` 作为不可创建的根目录 | Windows 无此语义，该字面量会变成可创建的盘符相对路径，断言直接反转 | 在测试根下建一个**普通文件**，再把它的子路径当目录。命中的是 `ensure_dir_exists()` 里 `stat` 成功但 `!S_ISDIR` 的那一行，两平台同一行 |
| `/proc/forbidden` 期望中间层与末层 mkdir 失败，`/proc` 期望 init 成功但 open 失败 | 只有 POSIX 才有「存在但不可写」的目录，跨机器无法保证 | 前两段同样用「父路径是文件」；第三段改为 init 成功于一个可写目录、随后 `rmdir` 掉该目录，再要求 open 失败 |
| `execl("./dump_pcm_test")` 依赖 cwd | Windows 需要绝对路径与 `.exe` | `iso_self_path()` 在 Windows 取 `GetModuleFileNameA`，POSIX 取 `argv[0]` |

## 踩过的坑：为什么不是 `_spawnve`

第一版用的是 CRT 的 `_spawnve(_P_WAIT, ...)`，在本机 MinGW 7.3 上 66/66 通过，在 runner 的 gcc 15.2 上却在 `_spawnve` 内部直接段错误（`RUN` 到崩溃之间只有 1 毫秒，进程根本没起来）。

原因是 MinGW 把 `_spawnve` 作为**静态运行时**编进可执行文件，于是它和其他本体内代码一样，落在 `-Wl,--wrap=` 的插桩范围内。这条耦合既观察不到也控制不了：本机 binutils 2.30 下没事，2.46 下崩。

换成 `CreateProcessA` + `WaitForSingleObject` + `GetExitCodeProcess` 后问题消失，而且这本来就是契约的字面表达。环境块也改为从 `GetEnvironmentStringsA()` 构造，不再依赖 CRT 的 `_environ`——同一个理由，少一层对运行时内部结构的假设。

## 刻意不做

- **Windows 上的覆盖率**：`gcov` 在 MinGW 里存在，但路径快照与 Linux 不可比；Linux 仍是覆盖率的唯一真相。
- **`test/windows_smoke.c` 保留**：它是十秒级、不依赖夹具的冒烟，全量套件跑挂时先跑它可快速区分「模块坏了」与「夹具坏了」。
- **子进程输出归属**：子进程日志仍与父进程混排。父进程打印 PASS/FAIL，归属由它负责。
- **超时与终止**：没有任何用例需要，契约里就不留这个面。

## 验证记录

| 路径 | 结果 |
|---|---|
| `sh test/run_full_windows.sh`（MinGW 7.3，直连 `gcc`） | 全绿，连续三次一致 |
| `USE_CMAKE=1 sh test/run_full_windows.sh` | 全绿 |
| `-Werror=implicit-function-declaration` 等严格标志编译 | 无错误（预演 CI 上 gcc 15.2 的严格化） |
| Linux `make run` | 由 CI 验证，本机无 Linux 工具链 |

`windows-latest` 上跑的是 gcc 15.2.0，比本机的 7.3 严格；CI 首次运行前，隐式声明一类问题已在本机用等价 `-Werror` 预演过。
