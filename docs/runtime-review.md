# Runtime review

Severity-ordered findings from a review of the Erelang interpreter runtime
(`src/runtime/`, `src/parser.cpp`, `src/lexer.cpp`, `src/optimizer.cpp`,
`src/ir.cpp`, `src/codegen_x64.cpp`, `src/typechecker.cpp`, `src/plugins.cpp`,
`src/main.cpp`, `src/obs_main.cpp`) performed alongside the runtime
folderization and bug-fix pass.

Severity legend:

- **P0**: crash, security, or undefined behavior
- **P1**: incorrect behavior (silent wrong results, dead APIs)
- **P2**: minor correctness / hygiene issues

All P0/P1/P2 items below have been fixed and verified with a rebuild and
smoke tests of `examples/`. See "Known remaining limitations" for items that
are deliberately out of scope or require larger rework.

---

## P0: crash / security / undefined behavior

### 1. `thread_remove(id, "force")` destroyed a joinable `std::thread`

`experimental/builtins/threads.cpp`

Erasing a `std::thread` while it is still joinable calls `std::terminate`.
`thread_remove(..., "force")` did exactly that for running workers.

**Fix**: force-removed workers are now parked (their native handle is moved
into `g_parkedThreads`) instead of destroyed, and `park_all()` joins every
remaining worker: including parked ones: before `Runtime::run()` returns.
See `threads.cpp:82-128`.

### 2. `system.execute` concatenated unquoted arguments -> command injection

`src/runtime/builtins/system.cpp`

Arguments were joined into one command line without quoting, so an argument
containing spaces or shell metacharacters could inject arbitrary commands.

**Fix**: each argument is quoted individually per the CRT's `CreateProcessW`
rules (`quote_windows_arg`, `system.cpp:66-98`), and the POSIX path now uses
`execv` (no shell) instead of `std::system`. `system.cpp:291`.

### 3. `ob_run_file` / `ob_collect_files` passed null to `fs::path` (UB)

`src/runtime/cabi_exports.cpp`

A null `main_file` reached `fs::path(...)` (undefined behavior), and `argv ==
nullptr` was not checked. `slurp_text` also silently returned `""` when a file
could not be opened.

**Fix**: null-check + `set_error_string` for `main_file` and `argv`
(`cabi_exports.cpp:147-183`, `203-215`); `slurp_text` now logs a warning on
open failure.

### 4. `GetModuleFileNameW` read out of bounds on paths longer than `MAX_PATH`

`src/main.cpp`, `src/obs_main.cpp`, `src/runtime/cabi_exports.cpp`

A fixed `MAX_PATH` stack buffer was passed, then `std::wstring(buffer, len)`
read past the end when the actual path was longer.

**Fix**: the buffer is grown (double-and-retry loop) so the length passed to
the `wstring` constructor never exceeds the allocation. `cabi_exports.cpp:51`.

### 5. Uncaught `std::stof` on `--ui-scale` -> `std::terminate`

`src/obs_main.cpp`

A malformed `--ui-scale` value threw `std::stof` out of `main`-adjacent code,
which aborts the process instead of reporting a CLI error.

**Fix**: wrapped in `try`/`catch` and reported like the other `cli::parse`
errors.

### 6. Hex literals containing `e`/`E` parsed as 0

`src/parser.cpp`

`0x1e`, `0xdead`, etc. fell through to `std::stod`, which treats the `e`/`E`
as a decimal exponent and yields `0` (then the double->int64 cast clamped it).

**Fix**: `0x`/`0X` prefixes are decoded with `std::stoll(text, nullptr, 16)`
before the float path is considered, and the double->int64 cast is range
checked. `parser.cpp:172-214`. `std::stoll` is also used in the numeric
literal / `sleep` parsing paths (`parser.cpp:1127`).

### 7. Dangling `@` at top level looped forever

`src/parser.cpp`

A trailing `@` with no following declaration was never consumed, so the
top-level parse loop spun without making progress.

**Fix**: a dangling attribute is consumed (with optional name and argument
list) so parsing always advances. `parser.cpp:1584-1599`.

### 8. Colon-method call hijacked ternary else-branches

`src/parser.cpp`

`x ? a : b()` was parsed as a method call `(x ? a : b)()` and failed with a
syntax error.

**Fix**: `:`+word+`(` is only treated as a colon-method call when not inside
a ternary else-position, tracked via `inTernaryThen_` (`parser.cpp:309-316`,
gated at `parser.cpp:423`).

### 9. `>>` / `<<` in generic type annotations threw

`src/parser.cpp`

The lexer emits a single `Shr`/`Shl` token for `>>`/`<<`, which broke depth
counting in `List<List<int>>` style annotations.

**Fix**: `Shr` counts as two `>`, `Shl` as two `<` during generic depth
parsing. `parser.cpp:2269-2271`.

### 10. Unterminated `#include "` swallowed the rest of the source

`src/lexer.cpp`

A `#include` whose closing quote was missing scanned to end-of-file, silently
discarding the remainder of the file.

**Fix**: the path scan stops at a newline and emits a `Bad` token.
`lexer.cpp:91`.

### 11. Optimizer folded floats with integer arithmetic + overflow UB + DoS

`src/optimizer.cpp`

Constant folding ran integer arithmetic on float literals (`1.5 + 2.5` -> `3`),
had signed-overflow UB in folded `+`/`-`/`*`, and `Pow` exponentiation could
loop unboundedly.

**Fix**: folding is skipped when either operand is a float literal, arithmetic
is checked (wraps to a non-folded result on overflow), and `Pow`/loops are
capped (exponent magnitude limit).

### 12. `collatz_len` / `collatz_sweep` signed overflow + unbounded cache

`src/runtime/builtins/math.cpp`

Signed overflow in the Collatz sequence could loop forever, and
`collatz_sweep` could `resize` an unbounded cache from the argument.

**Fix**: `unsigned long long` with an overflow guard; `limit` is capped
(`math.cpp:80`).

### 13. `BreakStmt` / `ContinueStmt` silently dropped by the IR lowerer

`src/ir.cpp`

The native (x64) path never emitted break/continue, so loops containing them
could not terminate in compiled output.

**Fix**: break/continue opcodes are now lowered; where a construct cannot be
lowered it is rejected with a clear error instead of being silently dropped.

### 14. `forEach` dereferenced `mc.args[0]` without a bounds check

`src/runtime/actions.cpp`

A for-each with no arguments could index `mc.args[0]` out of bounds.

**Fix**: `mc.args.empty()` is checked before access.

### 15. Division / modulo by zero silently returned 0; `ReturnStmt` swallowed errors

`src/runtime/eval.cpp`

`Div`/`Mod` by zero returned 0 instead of erroring, and `ReturnStmt`
evaluation wrapped everything in an empty `catch (...) { }` that discarded
runtime errors.

**Fix**: `Div`/`Mod` throw `"Division by zero"` / `"Modulo by zero"`
(`eval.cpp:316-322`); the empty catch was removed and the error is propagated.

### 16. Truthiness / numeric helpers inconsistent between `helpers.cpp` and `eval.cpp`

`src/runtime/helpers.cpp`, `src/runtime/eval.cpp`

`is_truthy("0")` returned `true` while `&&`/`||`/ternary treated `"0"` as
false; unary `Neg` truncated floats to ints; comparisons routed through
`to_int`, which broke string ordering.

**Fix**: truthiness is unified (single source of truth), `Neg` preserves
floats, and string comparisons are lexicographic.

---

## P1: incorrect behavior

### 1. All `monitor_*` APIs parsed `"monitor:N"` handles with `atoi` -> always 0

`experimental/builtins/monitor.cpp`

`atoi("monitor:3")` yields 0, so every monitor lookup used record 0: the
entire monitor API was dead.

**Fix**: the `"monitor:"` prefix is stripped before parsing
(`monitor.cpp:135-139`), mirroring `threads.cpp`.

### 2. Monitor races: re-fetch outside lock, non-atomic `intervalMs`, unguarded `stoul`

`experimental/builtins/monitor.cpp`

`monitor_add` inserted a record then re-fetched it outside the mutex
(null-deref race); `intervalMs` was read/written without synchronization; a
malformed interval could throw.

**Fix**: insert+fetch happen in a single lock scope, `intervalMs` is
`std::atomic<uint32_t>` with a 200 ms floor (`monitor.cpp:41-48`, `125`,
`235-242`), and the `stoul` is guarded.

### 3. `network` URL encoding, command quoting, and body length

`src/runtime/builtins/network.cpp`

- `url_encode` emitted single-digit percent escapes (`%A`).
- `execute_cmd` did not escape backslashes, breaking quoting (and enabling
  injection) for paths containing `\`.
- `http_post` truncated bodies larger than 4 GB via a `DWORD` cast.

**Fix**: `std::setw(2) + std::setfill('0')` (`network.cpp:397`);
backslashes are doubled per CRT rules (`network.cpp:292-308`); oversized
bodies are rejected instead of truncated (`network.cpp:127-129`). Response
reads are capped by `kMaxResponseBytes` (`network.cpp:100`, `158`).

### 4. Thread workers captured dangling `Runtime*` / `Program*` (use-after-free)

`experimental/builtins/threads.cpp`

Worker threads referenced the `Runtime`/`Program` that could be destroyed
before the worker finished, and join/`wait_all` could race with a concurrent
`remove(force)`.

**Fix**: workers never outlive the run: records stay joinable and
`park_all()` joins every worker (including parked force-removed ones) before
`Runtime::run()` returns (`threads.cpp:82-128`, `198-200`). Removal and join
are mutually exclusive under `g_threadMutex`.

### 5. Typechecker: unknown calls accepted in expression position; `parallel{}` never checked; builtins leaked across runs

`src/typechecker.cpp`

- Unknown calls / wrong arg counts were only diagnosed as statements, so
  `x = unknown_fn()` passed silently.
- The `ParallelStmt` arm used an impossible `is_same_v<T, shared_ptr<...>>`
  against a non-pointer variant, so `parallel{}` bodies were never checked.
- Module builtins accumulated across `check()` calls, leaking one module's
  builtins into the next.

**Fix**: expression-position validation emits `TC001` (`typechecker.cpp:355`,
`530`), the variant arm is fixed and `stmt->body` is checked
(`typechecker.cpp:800-801`), and `builtins_.clear()` runs at the start of
`check()` (`typechecker.cpp:934`).

### 6. Codegen: IR labels and `.LC` string labels collided across functions

`src/codegen_x64.cpp`

Each function reused IR branch labels (`.L1`, etc.), and `.LC<n>` string labels
were not globally unique, producing wrong branch targets / duplicated strings
in multi-action programs.

**Fix**: a per-function `fnPrefix` (sanitized function name) is prepended to
IR labels, `.LC` labels come from a shared global counter, and scratch labels
get a per-function sequence number (`codegen_x64.cpp:86-118`).

### 7. Runtime duplicated plugin logic; `initialize_environment` was a debug stub; state not cleared between runs

`src/runtime/core.cpp`

- ~200 lines of `seedPluginAliases` / `dispatchPluginHooks` were duplicated
  between `run()` and `run_single_action()`.
- `initialize_environment` was a leftover debug stub that spammed `cerr`.
- Global list/dict/ptr/file-handle containers were never reset between
  `run()` calls in the same process.
- `run_with_imports` ignored modules.

**Fix**: plugin seeding/dispatch is factored into private helpers; the
duplication is gone; `initialize_environment` is a documented no-op preserving
the public ABI (`core.cpp:146-151`); `reset_global_container_state()` runs at
the start of `run()` (`core.cpp:160-162`); `run_with_imports` merges module
globals/directives into the combined program (`core.cpp:130-143`).

### 8. Action method results via the magic `_` env var; `switch` only string equality

`src/runtime/actions.cpp`

Method calls return results by writing `_` into the environment, which can be
clobbered by nested calls, and `switch` matches by string equality only.

**Fix**: this is a known design limitation, documented here rather than
reworked mid-pass. See "Known remaining limitations".

### 9. Plugin manifest `<include>` could escape the plugin directory

`src/plugins.cpp`

An `<include>` path like `../../outside` escaped the plugin's base directory,
and tag matching was case-sensitive (dropping manifest content when the case
didn't match). `fs::directory_iterator` errors were swallowed silently.

**Fix**: include paths are normalized and verified to stay inside the base
directory; tags are lowercased on both sides (`plugins.cpp:56`, `86`); the
directory iterator's `error_code` is checked.

### 10. Optimizer: `-INT64_MIN` UB; `fold_block` skipped statement kinds

`src/optimizer.cpp`

Negating `INT64_MIN` is UB, and `fold_block` did not visit `DoWhileStmt`,
`RepeatStmt`, `TryCatchStmt`, `UnsafeStmt`, or `PointerSetStmt`.

**Fix**: checked negation; the missing statement kinds are now folded.

### 11. Typechecker: TC120 false positives on interpolation and call arguments

`src/typechecker.cpp`

The unused-variable pass only marked an identifier as used when it appeared
as a bare `ExprIdent`. Reads inside string interpolation (`"hi {name}"`) were
invisible because interpolation is a plain `ExprString`, and arguments of
expression-level calls (`string out = sys.cmd(script)`) were never walked, so
`name`, `script`, `file`, etc. were reported as unused.

**Fix**: `mark_interpolation_uses` extracts word tokens from `{...}` groups and
marks any that resolve to a declared variable; `FunctionCallExpr` now walks its
arguments before resolution. Genuinely unused variables still warn, and
previously-passing programs produce no new diagnostics.

---

## P2: minor correctness / hygiene

- **`src/parser.cpp`**: `08`/`09` literals are rejected (no octal
  misread) (`parser.cpp:190`); `sleep` accepts hex/float durations
  (`parser.cpp:1127`); `new Foo().bar` applies postfix after the `new`
  expression; `for (auto& x : items)` accepts `&`/`*` element qualifiers;
  `is_base_of` no longer discards its operand; implicit action bodies no
  longer consume an enclosing block's `}` (`parser.cpp:774-817`); unbounded
  recursion is stopped by `kMaxParseDepth` (2048) + `ParseDepthGuard`
  (`parser.cpp:12-23`, guards at `305`, `973`).
- **`src/lexer.cpp`**: `\uXXXX` escapes decode to real UTF-8 (not one byte);
  character literals with more than one code point (`'ab'`) are rejected via
  `utf8_codepoint_count` (`lexer.cpp:20`, `361`); duration lexing stops at
  word boundaries (`5seconds` is not a duration); dead `canEXPR()` removed.
- **`src/runtime/features/serialization.cpp`**: control characters
  (0x00-0x1F) are escaped as `\u00XX` with `\b`/`\f` shortcuts
  (`serialization.cpp:10-25`); `\uXXXX` escapes decode to UTF-8
  (`parse_string`, `serialization.cpp:81`); malformed input rolls back
  partially-parsed dict entries instead of leaving corruption
  (`serialization.cpp:156`, `174`).
- **`src/runtime/builtins/data.cpp`**: `std::stoi` on malformed handles no
  longer throws (`parse_data_handle`/`find_store`); `data_get`/`data_has`/
  `data_keys` no longer auto-create empty stores; `data_save` checks the
  output stream.
- **`src/runtime/builtins/binary.cpp`**: `bin_from_hex` rejects
  non-hexadecimal digits and odd-length input instead of producing garbage;
  handle parsing is exception-safe.
- **`src/runtime/builtins/system.cpp`**: `widen` does proper UTF-8->UTF-16
  conversion (was a byte copy); pipe reads are bounded by `kPipeReadTimeout`
  (60 s) with a drain grace period after process exit (`system.cpp:186`,
  `221`).
- **`src/runtime/builtins/crypto.cpp`**: `g_xor_state` is
  `std::atomic<uint32_t>` (relaxed), removing a data race.
- **`src/runtime/builtins/math.cpp`**: `add`/`sub`/`mul` use
  `checked_add`/`checked_sub`/`checked_mul` so `LLONG_MIN` ops don't overflow
  (`math.cpp:17-36`).
- **`src/runtime/builtins/regex.cpp`**: `regex_match` uses `std::regex_match`
  (full-string) instead of `std::regex_search` (substring).
- **`src/ir.cpp`**: nested `for (x in dict_new{...})` uses local dict entries
  to avoid iterator invalidation; `PointerSetStmt` is intentionally lowered to
  `nop` (no native pointer support: see limitations).
- **`src/codegen_x64.cpp`**: `idiv`/`mod` have a zero-divisor and
  `INT64_MIN / -1` overflow guard that emits a clean error + `exit` instead of
  a `SIGFPE` trap (`codegen_x64.cpp:275`, `347`, `365`); integer immediates
  with leading zeros are canonicalized so the assembler doesn't read them as
  octal.
- **`src/main.cpp`**: `--emit-asm` now emits real GAS x64 assembly
  (`emit_gas_win64_demo`) instead of a NASM stub.
- **`src/obs_main.cpp`**: `cmd /c` -> `cmd /S /C` so quoted paths with spaces
  work; `copy_odlls` reports `fs::copy_file` errors; manifest writes report
  the specific exception instead of an empty `catch (...)`; `std::ctime`
  output is stripped of its trailing newline; `write_if_changed` checks the
  output stream and reports open/write failures.
- **`src/runtime/cabi_exports.cpp`**: `slurp_text` warns when a file cannot
  be opened (see P0-3).

---

## Known remaining limitations

Deliberately not fixed in this pass (documented for a future session):

- **`PointerSetStmt` is a no-op on x64** (`src/ir.cpp`): there is no native
  pointer type in the codegen backend. `set_ptr`/`get_ptr` silently do
  nothing in compiled output; the interpreter path is unaffected.
- **`switch` only does string equality** (`src/runtime/actions.cpp`): no
  type/range/destructuring patterns yet.
- **Method results ride the magic `_` env var** (`src/runtime/actions.cpp`) : 
  nested calls can clobber it; a proper result-plumbing mechanism is a
  suggested improvement.
- **`exec` / `spawn` are intentionally shell-based** (`src/runtime/builtins.cpp`)
 : they take a raw command string by design (matches `os.exec` semantics).
  The P0 quoting fix applies to `system.execute`, which is the
  argument-array API.
- **`examples/debug/debug.elan` does not parse**: a one-line
  `hook onStart { print "..." }` body requires a statement separator before
  `}`, so the example fails with `Expected ;` at HEAD too (pre-existing, not a
  regression). The example needs its hook bodies reformatted with newlines.
- **`examples/checkfile.elan` has no `run` target**: `erelang checkfile.elan`
  reports `TC111: No run target set`; the file is a library/check file and
  needs a `run` directive (or be run via `obc --check`).
- **`initialize_environment` is a public-ABI no-op**: kept for compatibility;
  real environment seeding happens inside `run()` / `run_single_action()`.
- **`src/features/` is empty**: the folderization moved its contents to
  `src/runtime/features/`; the empty directory can be deleted once the move is
  committed.

---

## Suggested improvements

1. **Real `Value` type**: the string-backed `Value` is the root of most
   truthiness/numeric/comparison inconsistencies. Introduce a tagged union
   (`int64_t` / `double` / `string` / `bool` / `null`) and delete the
   `to_int`-through-strings conversions.
2. **CTest golden harness**: add `add_test()` entries running
   `examples/*.elan` against checked-in `.expected` outputs so the smoke tests
   become part of the build.
3. **Per-run state reset**: `reset_global_container_state()` covers
   list/dict/ptr/file handles; also reset the experimental thread/monitor
   registries so repeated `run()` calls in one process are fully hermetic.
4. **Result plumbing instead of `_`**: return method results through a
   dedicated eval result slot rather than an env var that nested calls can
   clobber.
5. **`PointerSetStmt` decision**: either implement real pointer values in the
   x64 codegen or reject the construct at typecheck time with a clear
   diagnostic (currently a silent no-op).
6. **Remove the dead split-DLL legacy CMake config**: the `erelang_shared` /
   split-DLL target duplicates `erelang_static` and is not exercised by any
   build or test.
7. **Delete the empty `src/features/` directory** after the folderization move
   is committed, and add a CI check that `src/` contains no leftover
   `runtime_*`/`builtins/` files.
