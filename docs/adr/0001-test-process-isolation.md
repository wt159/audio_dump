# ADR 0001 — The PC suite isolates processes by re-executing itself

- Status: accepted
- Date: 2026-11-11
- Context: map #25, ticket #30

## Context

Fifteen of the 66 PC cases cannot assert anything in the process that runs the
rest of the suite. They probe module-wide state: the lazy singleton init, the
low-storage latch, the one-shot retention scan, init failure and retry. Running
them in the main process would either observe a module that is already
initialised or poison it for every later case.

The harness solved this with `fork()`: the child called `setenv()` and then
`execl()` of the same binary, and the parent read the exit status through
`waitpid()`. That is a POSIX process model. Windows has none of the three, and
MinGW-w64 offers no usable stub — `fork` is not even declared.

## Decision

Express the contract, not the mechanism: **spawn this same binary with a few
extra environment assignments, wait for it, read its exit code.** It lives in
`test/proc_iso.c` behind `iso_run()`.

Three consequences worth recording, because each rejects an obvious alternative:

1. **No fork emulation.** Every one of the 15 call sites is
   `fork → setenv → execl(self) → waitpid`; none reads the parent's memory after
   the fork. A spawn is the same thing with less machinery, and it is one
   function call on each platform (`execve` after `fork`, `CreateProcessA` on
   Windows)
   instead of a POSIX emulation layer nobody would trust.
2. **Environment passed explicitly, not inherited by mutation.** `iso_run()`
   builds an environment block from `environ` with same-named entries replaced.
   Appending would be a bug, not a shortcut: glibc's `getenv()` returns the first
   match, so an appended override would be invisible. The parent's environment is
   never mutated, so a failing case cannot leak `AUDIO_DUMP_*` into the next one.
3. **"No result" is a failure.** The old code ignored `fork()`'s return value;
   when a spawn failed, `waitpid()` returned 0 and the case passed with nothing
   having run. One case was silently green for exactly this reason on Windows.
   `iso_run()` therefore returns non-zero when it cannot spawn or wait, and
   `iso_ok()` demands an explicit normal exit.

Deliberately absent: handles, streaming output, timeouts, termination. Every
caller is spawn-then-wait and reads only an exit code. Unused surface in a test
harness becomes a thing that silently stops working.

## Consequences

- The same `test/main.c` compiles and runs on Linux and Windows; no case is
  skipped, and no case needs an `#ifdef` in its assertions.
- A child's log output is still interleaved with the parent's. That is accepted:
  the parent prints the PASS/FAIL line and owns attribution.
- The layer is test-only. Production code gained nothing, and `src/` still has no
  process API.
- Anything that needs a *fresh* process on Windows must go through `iso_run()`;
  adding a second spawning mechanism would recreate the portability problem.
