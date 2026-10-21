# 2026-10-21: Runtime performance pass and sized collections

## Summary

Two goals for this session:

1. Review the C++ interpreter and make it fast.
2. Let scripts give a collection a size: `Array<int> A {6} = [1, 2, 3];`.

Both are done. The runtime is 4x to 13x faster depending on the workload, and the
self-host compiler (`compiler/main.elan`) runs 13x faster with byte-identical output.
Along the way about a dozen real bugs were found and fixed. Nothing is committed yet.

The root problem behind most of the slowness: the runtime is a string interpreter
dressed up as a typed one. Values were turned into text and parsed back at almost
every boundary, and every block and every call copied or scanned whole variable maps.

## Results

Release build, best of 3 runs, about 19 ms of process startup subtracted.
"Before" is a clean build of `HEAD` with its original CMake flags.

| Benchmark (`bench/`) | Before | After | Speedup |
|----------------------|--------|-------|---------|
| `loop.elan`: int loop, 300k iterations | 801 ms | 69 ms | 11.6x |
| `fib.elan`: recursive `fib(20)` | 65 ms | 14 ms | 4.6x |
| `list.elan`: 50k push, iterate, index | 376 ms | 51 ms | 7.4x |
| `builtin.elan`: 100k `string.len` calls | 458 ms | about 70 ms | 6.5x |
| `compiler/main.elan` (self-host) | 3960 ms | 304 ms | 13x |

`dict.elan` could not run before at all (`map[key] = v` was not supported).

## 1. Build configuration (`CMakeLists.txt`)

**What was wrong**

- Release used `-Os` / `/O1` (optimize for size). For an interpreter loop that is the
  wrong trade.
- The MSVC LTO check sat inside the non-MSVC branch, so it could never run.
- `BUILD_SHARED_RUNTIME` defaulted to `ON` even though the comment said "Default OFF",
  so every source compiled twice (static lib plus `erelang.dll`), while the runner only
  links the static lib.
- `add_compile_options(-Wall -Wextra ...)` was called after all targets were created, so
  warnings were never actually enabled.
- `CMAKE_BUILD_TYPE` checks do not work with multi-config generators (Visual Studio).

**What changed**

```cmake
add_compile_options($<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-ffunction-sections> ...)
option(BUILD_SHARED_RUNTIME "..." OFF)
foreach(_ob_warn_target IN ITEMS erelang erelang_runner obc)
  target_compile_options(${_ob_warn_target} PRIVATE -Wall -Wextra)
endforeach()
```

Release flags now use generator expressions, so they are correct for every generator.
Warnings now apply, which makes about 40 old warnings visible. Only one was new from this
session and it is fixed.

## 2. Block scope cleanup (`src/runtime/actions.cpp`, `include/erelang/value.hpp`)

**What was wrong**

`exec_block` copied every variable name into an `unordered_set` on entry, so it could
release owned handles and close files bound to names the block introduced. This ran for
every block, including every loop iteration, and the env also held every global.

**What changed**

`ValueMap` now records keys as they are inserted (`insertLog`). A block remembers the log
size on entry and on exit only looks at keys added since then. Surviving new names stay in
the log, so an enclosing block still treats them as its own (same semantics as before).

```cpp
void Runtime::exec_block(const Block& b, ...) const {
    const std::size_t mark = env.vars.insertLog.size();
    ...
    release_block_scope(env, mark);
}
```

## 3. Calls no longer copy all globals (`eval.cpp`, `actions.cpp`, `builtins.cpp`)

**What was wrong**

About 13 call paths did this before running a callee:

```cpp
for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
```

`globalVars_` also holds two entries per enum variant and every module alias, so each call
paid for all of them.

**What changed**

The copies are gone. A new `env_find` returns a pointer and falls back to `globalVars_`.
`env_set` never inserts a global name as a callee local, otherwise block exit would treat a
global holding a `heap<T>` as scope-owned and release it.

## 4. Slots disabled (`eval.cpp`)

**What was wrong**

Each call ran `prepare_action_slots`: walk the action body, build a name vector, a set and
a name-to-index map, then copy values in. But every slot access still hashed the name
through `slotIndex`, and every write went to both the slot and `env.vars`. The slots saved
nothing and cost an AST walk plus three containers per call.

They also caused a bug: moving a `heap<T>` cleared the variable in `env.vars` but the stale
slot kept the old value, so use-after-move was not detected (`tests/memory/heap_move.elan`
printed `use-after-move-missed`; it now prints `use-after-move` as intended).

**What changed**

`prepare_action_slots` now just turns slots off. `env.vars` is the single store.

## 5. Expression evaluation (`eval.cpp`, `bytecode.cpp`)

**What was wrong**

- `eval_value` built a fresh bytecode `Chunk` for every literal, unary and binary
  expression on every evaluation, ran it, and threw it away. `+ - * / == !=` were never
  compiled anyway, so for `sum + i * 2` it tried and failed at each level.
- Every binary operation copied `lastReturnFields_` (a whole `unordered_map`) twice,
  stringified the left side, and probed for pointer, unit and struct semantics.
- Reading a variable did up to 7 hash lookups and allocated a display string to check for
  `"struct:"`.
- Member access parsed a type string inside `try/catch` on every `p.x`.

**What changed**

- The per-eval chunk compile is removed.
- Binary operations take a fast path when both sides are numbers or bools:

```cpp
Value left = eval_value(*binary.left, env);
if (is_scalar(left)) {
    Value right = eval_value(*binary.right, env);
    if (is_scalar(right)) return apply_binary(to_value_bin_op(binary.op), left, right);
    ...
}
```

  Everything else goes through `eval_binary_slow`, which is the old code unchanged.
- Identifier reads use one `env_find` lookup and a kind check (`value_is_struct_marker`).
- Type strings are only parsed when the name actually contains `<`.

## 6. Action lookup (`actions.cpp`, `core.cpp`, `runtime.hpp`)

**What was wrong**

`find_action` scanned every action linearly, took a mutex twice and built a `"::name"`
string, on every call. It also returned pointers into `dynamicActions_` (a `std::vector`)
that a reload would `erase` from, leaving callers with dangling pointers.

**What changed**

- `index_actions` builds a name map and a `::` suffix map at the start of each run. A
  guard clears it when the run ends so it never outlives its `Program`.
- The mutex is only taken when dynamic actions exist (`hasDynamicActions_`).
- `dynamicActions_` is now a `std::list`, so loading and unloading never moves other actions.

## 7. Typed list and map storage (`runtime_helpers.hpp`, `helpers.cpp` and about 20 call sites)

**What was wrong**

```cpp
std::unordered_map<int, std::vector<std::string>> g_lists;
std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;
```

Every element was stored as text and parsed back on every read. List literals went through
`eval_builtin_call("list_new")`, which returned a string handle that was parsed again.

**What changed**

- `g_lists` holds `std::vector<Value>` and `g_dicts` holds `unordered_map<std::string, Value>`.
- Places that only have text use `value_from_legacy_string`, so `"5"` still reads back as
  an int, exactly as before.
- List and map literals are built directly.
- `a[i]`, `m.field` and `for (x : list)` return stored values with no parsing.
- All list and map methods moved into one function, `Runtime::collection_method`
  (`eval.cpp`). It is used by both `a.push(x);` statements and `n = a.len()` expressions.
  Arguments are evaluated once. Before, `a.push(f())` ran `f()` twice.
- `capacity`, `reserve` and `shrink_to_fit` are real now (they were stubs).
- `map[key] = value` works (it threw "index assignment requires buffer or list").

## 8. Builtin calls (`builtins.cpp`, `eval.cpp`)

**What was wrong**

`eval_builtin_call` is one ~2600 line function with about 260 `nameStr == "..."` checks.
`std::string == "literal"` compares bytes before lengths, so each miss cost a few
nanoseconds and a common builtin paid about 1 microsecond before it was even reached.
Before getting there, `FunctionCallExpr` also evaluated every argument and stringified the
receiver to check for methods, even for module prefixes such as `string.len`.

**What changed**

- The 33 legacy-name rewrites (`fopen` to `file_open`, `hashmap_*` to `dict_*`, ...) are one
  static hash map.
- The chain compares against a `std::string_view`, which checks length first, so most
  misses cost one integer compare.
- Method dispatch is skipped when the receiver name is not a bound variable.

## 9. Strings are shared (`value.hpp`)

**What was wrong**

`Value` held `std::string* str` and deep-copied it on every copy (two heap allocations).
Values are copied constantly (lookups, arguments, returns, list reads).

**What changed**

```cpp
std::shared_ptr<std::string> str;
```

Copying a string value is now a refcount bump. `string_mut()` copies first if the string
is shared (copy-on-write). This alone took the self-host compiler from 434 ms to 304 ms.

## 10. Smaller performance fixes

| File | Problem | Fix |
|------|---------|-----|
| `src/lexer.cpp` | Copied the whole keyword set (40+ strings) for every identifier | Reference to a static set |
| `src/parser.cpp` | `a < b` scanned to end of file looking for a closing `>` | Stop at newline, `;`, `{`, `}` |
| `regex.cpp` | `std::regex` compiled on every call | Bounded cache of compiled patterns |
| `memory.cpp` | `p + 1` copied the whole byte buffer behind the pointer (O(n^2) to walk a buffer) | Views share cells, no copy |
| `actions.cpp` | `print` flushed with `std::endl` every line | `'\n'`; stdout is flushed before shell commands |
| `actions.cpp` | `x += 1` allocated two AST nodes per execution | Stack-built node |
| `actions.cpp` | `return` allocated an empty string every time | Only when there is no value |

## 11. Sized collections (the new feature)

**Syntax**

```elan
Array<int> a {6} = [1, 2, 3];   // [1, 2, 3, 0, 0, 0]
Array<string> names {3};        // ["", "", ""]
Array<double> ds {n * 2};       // size can be any int expression
Map<string, int> m {64};        // capacity hint, still grows
```

**Rules**

- An array has exactly N elements. The initializer comes first, the rest is the zero value
  of the element type.
- `push`, `pop`, `insert`, `erase` and `clear` throw, for example
  `cannot push to a fixed-size array (length 6)`.
- `a[i] = v` outside the array throws `list index 6 out of bounds (length 6)`.
- `Array<int> b {4} = other;` copies `other`, so the source keeps its own length.
- On `Map` and `Set`, N only reserves space.

**New diagnostics**

| Code | Meaning |
|------|---------|
| `TC171` | Size is not an `int`, or is a negative constant |
| `TC172` | A literal has more elements than the declared size |
| `TC173` | `{N}` used on a type other than Array, Map or Set |

`Array<int> A {6} = [1,2,3,4,5,6,7];` is a TC172 error. If N is only known at run time,
the same check runs at run time.

**Where it lives**

- `include/erelang/parser.hpp`: `LetStmt::sizeExpr`.
- `src/parser.cpp`: after the declared name, `{ expr }` is parsed as the size. `{` there was
  never valid before, so nothing existing changes meaning.
- `src/typechecker.cpp`: TC171 to TC173. A sized declaration may omit the initializer
  (an empty `[]` used to be TC055).
- `src/runtime/actions.cpp`: `apply_declared_size` pads, marks the list fixed
  (`g_fixedLists`) or reserves map/set capacity.
- `erevos-language/src/analysis.ts`, `symbols.ts`: declaration regexes accept `{N}`.
- Docs: `docs/collections.md` (new "Sized collections" section), `docs/diagnostics.md`,
  `docs/syntax.md` (replaced the stale `list:T` / `map:K,V` rows).

## 12. Bugs fixed

| Where | What was wrong | Fix |
|-------|----------------|-----|
| `typechecker.cpp` `resolve_type` | An early `== "array"` check returned `array<any>` for every `Array<T>`, so element types were never checked and `a[0] + 1` was a type error | Resolve the element type; unresolvable ones (generic `T`) stay `array<any>` |
| `typechecker.cpp` IndexExpr | `a[i]` and `m[k]` were always `unknown` | `Array<T>` index is `T`, `Map<K,V>` index is `V` |
| `typechecker.cpp` | `a.len()`, `m.has(k)` and similar were untyped, so `int n = a.len();` failed | Types for all list and map methods |
| `typechecker.cpp` `pass_collect` | Called `init_builtins()` a second time, overwriting module signatures such as `ws.recv_timeout` | Removed the second call |
| `typechecker.cpp` | The implicit `_`, `res`, `req` made `string res = ...` a redeclaration error | Body gets its own scope; implicit names may be reused |
| `typechecker.hpp` | `ScopeManager::Guard` was copyable, and a copy pops the scope twice | Move-only |
| `obs_main.cpp` | Merging `#include`d files dropped structs, enums, aliases, traits and externs, so an included struct was an unknown type | One `merge_loaded_programs` helper (was copy-pasted twice) that keeps them |
| `eval.cpp` | `string k = "k{i}"` did not interpolate (only `print` and arguments did), against the docs | Interpolate in every string literal |
| `eval.cpp` | Values passed into builtins were wrapped as string literals, so user text containing `{name}` got interpolated | `ExprString::raw` flag for those wrappers |
| `value.cpp`, `bytecode.cpp` | Signed overflow was undefined behavior; `INT64_MIN / -1` crashes on x86 | Two's-complement wrapping, guarded `/ -1` and `% -1` |
| `actions.cpp` ForIn | `for (x : list)` iterated the live vector while the body could push to it | Index loop over the starting length; map loops use a snapshot |
| `network.cpp` | HTTP head read one byte per `recv` with no size limit; a malformed `Content-Length` threw inside the handler thread | Buffered `SocketReader`, 8 KB line cap, 200 header cap, `strtol` |
| `parser.cpp` | `static Token greaterTok` was written on every `peek`, a data race between threads | Built once, const |
| `memory.cpp` | `alloc<T>(n)` had no overflow check on `n * sizeof` | Size limit with a clear error |
| `obs_main.cpp` | `--help` ran plugin discovery first (scans and creates plugin folders) | `--help` handled first |

## 13. Behavior changes script authors may notice

- `{x}` interpolates in all string literals. Braces that are not a valid expression (JSON
  text, a lone `{`) stay as written.
- Arrays are really typed now, so old scripts can get new, correct type errors.
- List elements keep their type. A string variable holding `"5"` stays a string when pushed.
  String literals still parse the old way (`["5"]` holds the int 5).
- `print` output is buffered. Order relative to stderr can differ when both go to one pipe.
- Integer overflow wraps instead of being undefined.
- `examples/random/ws_client_smoke.elan` and `tests/std/stdio.elan` print their stderr line
  in a different position. This is stream ordering only, the new order matches the program.

## 14. Verification

- `tests/run.ps1`: 74 of 74 pass on Debug and Release. 68 were there before, 6 are new:
  `tests/std/array_sized.elan`, `tests/std/map_sized.elan`,
  `tests/errors/array_sized_{overflow,type,runtime}_neg.elan`,
  `tests/modules/include_types.elan` (helper in `tests/modules/lib/`, which the runner now skips).
- Positive tests only check the exit code, so stdout of all 144 test and example files was
  saved before the work and diffed after every phase. The only differences are the
  `heap_move` fix, stream ordering, and time or random output.
- `compiler/main.elan` output is byte-identical to the original build.
- The extension type-checks with `tsc --noEmit`.
- HTTP server smoke test: GET and a 20 KB POST, same output as the original build.

## 15. Not done (open items)

- **Threads**: `parallel {}` and async still share `globalVars_`, `lastReturnFields_` and all
  list/map/pointer registries without locks. Writing shared state from two threads is a data
  race. A real fix needs locking or per-thread state across the runtime.
- **Bigger speedups** left for later: resolving variables to fixed slots ahead of time, and a
  real builtin dispatch table (the 2600 line chain is only made cheaper, not replaced).
- `src/main.cpp` (the `obc` driver) still has its own merge that drops included structs.
- `print a` on a list prints the handle (`list:1`), not the contents.
- Reading `a[i]` out of range returns `0` instead of an error (writes do error).
- `res.html(...)` in HTTP handlers returns a default page. This happens on the original
  build too, so it was left alone.

## Build notes

- Release: `cmake -S . -B build-rel -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_RUNTIME=OFF`
- Every `erelang_runner` build copies the exe to `%LOCALAPPDATA%\Erelang\bin` (the one on
  PATH). The session ended with a normal `build/` Debug build so that copy is current.
- Benchmarks: run each file in `bench/` with `Measure-Command`.
