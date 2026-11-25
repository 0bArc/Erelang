# 2026-11-24 Fast path: specialized ops, direct dispatch, cheaper calls and lists

## What was wrong

The bytecode loop in `src/runtime/fast_vm.cpp` ran at about 2.5 ns per op:

- Every arithmetic op and compare-jump went through two switches: the op switch, then a second
  one on the operator inside `int_binary` / `int_compare`.
- GCC merged the computed-goto style dispatch into one shared indirect jump.
- Each call to a compiled action looked up two `thread_local` objects (`arena()` and
  `empty_env()`), which are function calls on MinGW.
- Each `xs[i]`, `xs[i] = v`, `xs.len()`, `xs.push(v)` and for-each step looked the list up in the
  `g_lists` hash table, and methods went through `collection_method`'s string comparisons.

## What changed

- `fast_vm.cpp`
  - The op list is one X-macro (`ERELANG_FAST_OPS`) that builds the enum and the dispatch table.
  - `Compiler::specialize` (run in `finish()` after register mapping) rewrites `Bin` with `+ - *`
    into `AddInt` / `SubInt` / `MulInt`, and compare-jumps into `JmpLt` ... `JmpNe`. `K` forms are
    used when the right operand is an int constant register that no op writes. A false-jump
    becomes the inverse int comparison; operands that are not both ints use the original
    operator and direction (kept in `sub`), so float and NaN behavior is unchanged.
  - A loop step (`IncInt` or `AddIntK`) right before a `JmpLt` / `JmpLe` is fused
    (`IncIntLt`, `IncIntLe`, `AddIntKLt`, `AddIntKLe`): for ints the jump is decided in the
    same op; otherwise the jump runs next as a normal op.
  - With GCC and Clang each op dispatches the next one directly (`goto *labels[op]`); other
    compilers keep the switch. Generic fallbacks are out of line (`bin_generic`, `cmp_generic`).
  - `+ - *` on mixed int / float compute inline.
  - The frame arena is passed through `exec` and `run_func`, and holds the empty `Env`.
    Releasing a frame resets registers without `Value` assignment.
  - Each frame caches the last list it looked up. `g_lists` entries are only removed by
    `Runtime::run`, and the cache is dropped before any op that may run other code (calls,
    builtins, the tree-walker operator path). `xs[i]`, `xs[i] = v`, list for-each and
    `len` / `push` / `get` use it inline; out-of-range, missing-list and fixed-size cases use
    the existing helpers, so the results and errors are the same.
- `CMakeLists.txt`: `fast_vm.cpp` is built with `-fno-gcse -fno-crossjumping` on GCC, which
  keeps one dispatch jump per op (GCC's documented advice for computed goto).
- `eval.cpp`, `include/erelang/runtime.hpp`: `list_method_name` exposes the list alias mapping
  (`size` is `len`) to the compiler.
- Test: `tests/semantics/fast_specialized.elan` (counter that becomes a float in a fused loop,
  string `+` and comparisons, mixed compares in both jump directions, wrapping multiply,
  two lists alternating with a call between, out-of-range reads).
- Docs: `docs/pages/examples/bench.md` sample numbers.

## Behavior changes

None intended. Only speed.

## Verification

Release build, best of 3, total process time including about 15 ms of startup, scripts in the
session scratchpad (not committed):

| Workload | Before | After |
|----------|--------|-------|
| `i = i + 1` to 1e7 | 56 ms | 44 ms |
| `i = i + 1` to 1e8 | 466 ms | 300 ms |
| `sum = sum + i * 2 - 1` 1e7 | 161 ms | 85 ms |
| `acc = acc + i * 0.5` 5e6 | 61 ms | 45 ms |
| `fib(30)` | 191 ms | 104 ms |
| nested `for` 3e6 with `%` and `if` | 63 ms | 36 ms |
| 1e6 `push` then 1e6 `xs[k]` sum | 136 ms | 55 ms |

- `cmake --build --preset release` and `--preset debug` (`erelang_runner`): success, no
  warnings from `fast_vm.cpp`.
- `pwsh tests/run.ps1` (debug): pass=101 fail=1. The failure is
  `tests/semantics/no_semicolons.elan`, failing as before this change.
- Every `.elan` under `tests/` and `examples/` (203 files) with the release build from before
  this change and the new one, comparing exit code, stdout and stderr: 3 differ, all random or
  time output (`examples/random/crypto_sha256.elan`, `examples/types/bench.elan`,
  `tests/std/time.elan`).
- The new test's actions were confirmed to compile (temporary trace, removed), and it passes on
  both builds.
- `compiler/main.elan`: identical output on both builds.
- Discord and Docker plugin tests (release): all pass. `examples/bench/run.ps1` runs.

## Not done

- Clang (macOS) and Linux GCC were not built here; CI covers them. The dispatch code uses only
  `&&label` / `goto *`, which both support.
- Structs, entities, closures, `try`, `match`, sets and most builtins still run on the
  tree-walker; that is where most remaining time goes in real programs.
- Values are 32 bytes with a `shared_ptr` string; a smaller value (NaN-boxing or a tagged
  pointer) would make every register move cheaper but touches the whole runtime.
- No machine-code generation yet.
