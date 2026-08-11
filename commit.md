# Erelang Language Maturation — Commit Summary

## Overview

Three pillars of change:
1. **Clean API surface** — semantic modules (`fs.read`, `net.get`, `crypto.hash`), handle methods (`list.push`), type constructors (`int("42")`), while keeping fundamental operations fundamental (`print`, `+`, `-`)
2. **New infrastructure** — debug module, process protection, thread pool control, performance profiling, compiler directives, compiler flags
3. **Strict type checking** — no silent fallback to `any`, compile-time type errors, `any` only when explicitly requested by the programmer

---

## Phase-by-Phase Changes

### Phase 1: Handle-Type Method Dispatch
**File:** `src/runtime/actions.cpp`

Extended the existing dynamic method dispatch (List/Dict at `actions.cpp:603-950`) to **Set**, **Queue**, **StrBuf**, **Ptr**, and **File** handles. Each handle prefix gets its own dispatch block:

| Handle Prefix | New Methods |
|---|---|
| `set:` | `add`, `has`, `remove`, `size`, `values`, `union`, `intersect`, `diff` |
| `queue:` | `push`, `pop`, `peek`, `len`/`size`, `clear` |
| `strbuf:` | `append`, `clear`, `len`/`size`, `to_string`, `free`, `reserve` |
| `ptr:` | `get`, `set`, `valid`, `free` |
| `file:` | `read(count?)`, `write`, `seek(offset, whence?)`, `tell`, `flush`, `close` |

### Phase 2: Type Constructors & Core Utils
**File:** `src/runtime/builtins.cpp`

- **Type constructors** added as aliases:
  - `int(x)` → wraps `toint`
  - `float(x)` → wraps `tofloat`
  - `string(x)` → wraps `tostr`
  - `bool(x)` → wraps `tobool`

- **Type-check methods** added:
  - `int.is(x)` → returns `"true"`/`"false"`
  - `float.is(x)` → returns `"true"`/`"false"`
  - `string.is(x)` → returns `"true"`/`"false"`

- **26 new module aliases** (always available, no import required):

| Module | Aliases Added |
|---|---|
| `time` | `time.now_iso`, `time.now_ms`, `time.rand_int`, `time.uuid` |
| `char` | `char.is_digit`, `char.is_space`, `char.is_alpha`, `char.is_ident_start`, `char.is_ident_part` |
| `env` | `env.get` (was `env`), `env.load_dotenv` (was `dotenv_load`) |
| `json` | `json.encode` (was `to_json`), `json.decode` (was `from_json`) |
| `lang` | `lang.name`, `lang.version`, `lang.about`, `lang.limitations` |
| `plugin` | `plugin.core`, `plugin.core_files`, `plugin.core_keys` |
| `io` | `io.read_line`, `io.stdin`, `io.stderr`, `io.prompt`/`io.input` |

### Phase 3: Data & Binary Module Aliases
**File:** `src/runtime/imports.cpp`

**`builtin/data`** now resolves both old flat names and new dotted names:

| New Syntax | Maps To |
|---|---|
| `data.new()` | `data_new` |
| `data.set(h, k, v)` | `data_set` |
| `data.get(h, k)` | `data_get` |
| `data.has(h, k)` | `data_has` |
| `data.keys(h)` | `data_keys` |
| `data.save(h, path)` | `data_save` |
| `data.load(path)` | `data_load` |

**`builtin/binary`** now resolves both old flat names and new dotted names:

| New Syntax | Maps To |
|---|---|
| `bin.new()` | `bin_new` |
| `bin.from_hex(s)` | `bin_from_hex` |
| `bin.len(h)` | `bin_len` |
| `bin.to_hex(h)` | `bin_hex` |
| `bin.push_u8(h, v)` | `bin_push_u8` |
| `bin.get_u8(h)` | `bin_get_u8` |

### Phase 4: Thread Rework + Debug Module
**Files:** `src/runtime/imports.cpp`, `experimental/builtins/threads.cpp`, `src/runtime/builtins.cpp`

**Thread module** reworked with new modular aliases:

| New Syntax | Maps To / Behavior |
|---|---|
| `thread.spawn(action, ...args)` | `thread_run` |
| `thread.spawn_detached(action, ...args)` | `thread_run` with detach=true |
| `thread.sleep(ms)` | Added to builtins.cpp |
| `thread.result(id)` | Added to builtins.cpp (stub) |
| `thread.join(id)` | `thread_join` |
| `thread.join_timeout(id, ms)` | `thread_join_timeout` |
| `thread.kill(id)` | `thread_purge` |
| `thread.active()` | `thread_count` |
| `thread.wait_all([timeout])` | `thread_wait_all` |
| `thread.done(id)` | `thread_done` |
| `thread.list()` | `thread_list` |
| `thread.pool.max(n)` | Stub (typechecker validates) |
| `thread.pool.stop()` | Stub (typechecker validates) |

All thread dispatch functions in `experimental/builtins/threads.cpp` updated to handle dotted names.

**Debug module** — 17 new builtins in `builtins.cpp`:

| Builtin | Description |
|---|---|
| `debug.section(label)` | Prints section header |
| `debug.assert(cond, msg?)` | Asserts condition, aborts on fail |
| `debug.assert_eq(a, b, msg?)` | Asserts equality, aborts on fail |
| `debug.log(msg)` | Prints log message |
| `debug.trace(cat, msg)` | Prints trace message with category |
| `debug.warn(msg)` | Prints yellow warning |
| `debug.error(msg)` | Prints red error |
| `debug.ok(msg)` | Prints green success |
| `debug.dump()` | Dumps all environment variables |
| `debug.dump_var(name)` | Dumps single variable with type info |
| `debug.stack()` | Prints stack info (stub) |
| `debug.heap()` | Prints container stats |
| `debug.timer_start(label)` | Starts named timer |
| `debug.timer_stop(label)` | Stops timer, prints elapsed ms |
| `debug.breakpoint()` | Native debugger break |
| `debug.level.set(n)` | No-op (typechecker validates) |
| `debug.guard(cond)` | No-op (typechecker validates) |

**Monitor module** aliases wired:

| New Syntax | Maps To |
|---|---|
| `monitor.add(...)` | `monitor_add` |
| `monitor.remove(...)` | `monitor_remove` |
| `monitor.list(...)` | `monitor_list` |
| `monitor.info(...)` | `monitor_info` |
| `monitor.last_change(...)` | `monitor_last_change` |
| `monitor.set_interval(...)` | `monitor_set_interval` |

### Phase 5: Process Protection + Regex Extension
**Files:** `src/runtime/imports.cpp`, `src/runtime/builtins/system.cpp`, `src/runtime/builtins/regex.cpp`

**`builtin/process`** module added (maps to existing system builtins):

| New Syntax | Maps To / Behavior |
|---|---|
| `proc.execute(bin, args...)` | `system.execute` |
| `proc.shell(cmd)` | `system.cmd` |
| `proc.spawn(bin, args...)` | `system.execute` |
| `proc.output()` | `system.output` |
| `proc.exit_code()` | `system.last_exit` |
| `proc.opts()` | Stub — returns options handle |
| `proc.kill(pid)` | Stub |
| `proc.wait(pid, ms)` | Stub |
| `proc.alive(pid)` | Stub |

**Regex module** extended with 6 new builtins:

| New Builtin | Description |
|---|---|
| `re.find_all(text, pat)` | Returns list of all matches |
| `re.split(text, pat)` | Splits by regex, returns list |
| `re.capture(text, pat)` | Captures groups from match |
| `re.group(idx)` | Returns captured group by index |
| `re.compile(pat)` | Compiles pattern, returns id |
| `re.free(id)` | Frees compiled pattern |
| `re.test(text, pat)` | Alias for `re.match` |

### Phase 6: Performance Split + Compiler Directives + Build Flags
**Files:** `src/runtime/builtins/performance.cpp` (new), `src/obs_main.cpp`, `src/runtime/imports.cpp`, `CMakeLists.txt`

**Runtime Performance API** (`builtin/performance`):

| Builtin | Description |
|---|---|
| `perf.profile.begin(label)` | Start named profiling region |
| `perf.profile.end(label)` | End profiling region, returns elapsed ms |
| `perf.profile.duration(label)` | Cumulative ms for label |
| `perf.profile.calls(label)` | Call count for label |
| `perf.profile.report()` | Full profile dump |
| `perf.mem.usage()` | Current RSS (stub) |
| `perf.mem.peak()` | Peak RSS (stub) |
| `perf.gc.collect()` | Force GC cycle (stub) |
| `perf.gc.threshold(bytes)` | GC pressure threshold (stub) |
| `perf.gc.pause()` | Pause GC (stub) |
| `perf.gc.resume()` | Resume GC (stub) |

**Compiler directives** (parser-level `@attributes` — already supported, no code change):
- `@inline` — hint: flatten this call
- `@noinline` — hint: never inline
- `@hot` — optimize this block aggressively
- `@cold` — treat as cold code path

**Compiler flags** (added to `--compile`):

| Flag | Effect |
|---|---|
| `--lto` | Link-Time Optimization |
| `--strip` | Strip debug symbols from output |
| `--minify` | Strip whitespace/comments from embedded sources |
| `--gc-sections` | Drop unused function sections at link time |

Usage: `erelang --compile app.elan --strip --minify --lto --gc-sections`

### Phase 7: Strict Type Enforcement
**File:** `src/typechecker.cpp`

- `merge_inferred_type`: conflicting types now produce `"unknown"` (not `"any"`), triggering errors downstream
- `decl == "any"` now correctly maps expected type to `"any"` (not `"unknown"`)
- `generic_type_compatible`: `"unknown"` is never silently compatible — only explicit `"any"` passes
- Three lambda compatibility checks (LetStmt, SetStmt, ReturnStmt) reject `"unknown"`:
  - `is_compatible(...)` in `LetStmt`: `unknown` returns false
  - `assign_compatible(...)` in `SetStmt`: `unknown` returns false
  - ReturnStmt check: `t.name == "any"` (not `"unknown"`) as safe fallback
- **Binary operator type checking** strengthened:
  - `+`: requires int+int or string+string — type errors emit `TC011` with diagnostic hints
  - `-`, `*`, `/`, `%`: require int operands — errors emit `TC013`
  - `&&`, `||`: require bool operands — errors emit `TC014`
  - Comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`): unchanged (compare anything at runtime)

### Phase 8: Backward Compatibility
**Files:** `src/runtime/imports.cpp`, `src/runtime/builtins.cpp`

All old flat names remain as working aliases:
- `data_new`, `data_set`, etc. still work alongside `data.new`, `data.set`
- `bin_new`, `bin_push_u8`, etc. still work alongside `bin.new`, `bin.push_u8`
- `thread_run`, `thread_join`, etc. still work alongside `thread.spawn`, `thread.join`
- `monitor_add`, `monitor_remove`, etc. still work alongside `monitor.add`, `monitor.remove`
- `toint`, `tofloat`, `tostr`, `tobool` still work alongside `int()`, `float()`, `string()`, `bool()`

Deprecation warning mechanism (`warn_deprecated`) already exists in builtins.cpp — emits `[warn]` messages to stderr on first use of deprecated names.

---

## Files Changed

| File | Nature |
|---|---|
| `src/runtime/actions.cpp` | **Modified** — Added Set/Queue/StrBuf/Ptr/File handle dispatch (~200 lines) |
| `src/runtime/builtins.cpp` | **Modified** — Type constructors, type checks, debug module, thread helpers, module aliases (~100 lines added) |
| `src/runtime/imports.cpp` | **Modified** — All module resolve+bind entries for data, binary, threads, monitor, process, performance, regex (~120 lines) |
| `src/runtime/builtins/performance.cpp` | **New** — Runtime profiling, memory, GC API (~120 lines) |
| `src/runtime/builtins/regex.cpp` | **Modified** — Extended with find_all, split, capture, group, compile, free (~100 lines added) |
| `src/runtime/builtins/system.cpp` | **Modified** — Process protection aliases and stubs (~30 lines) |
| `experimental/builtins/threads.cpp` | **Modified** — Thread dispatch updated for dotted names (~30 lines) |
| `src/obs_main.cpp` | **Modified** — CompileOptions extended, CLI flags parsed, help text updated (~40 lines) |
| `src/typechecker.cpp` | **Modified** — Strict type enforcement: no silent any fallback, operator checking (~50 lines) |
| `CMakeLists.txt` | **Modified** — Added `performance.cpp` to build (~2 lines) |

---

## Module Map Reference

### Layer 1: Fundamental Operations (no namespace, always available)
`print`, `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `>`, `<`, `>=`, `<=`, `&&`, `||`, `!`, `??`, `?:`, `if`, `else`, `while`, `for`, `do`, `repeat`, `switch`, `case`, `return`, `break`, `continue`, `new`, `run`

### Layer 2: Core Types & Converters (always available)
`int(x)`, `float(x)`, `string(x)`, `bool(x)`, `int.is(x)`, `float.is(x)`, `string.is(x)`, `time.*`, `char.*`, `env.*`, `json.*`, `lang.*`, `plugin.*`, `io.*`

### Layer 2: Handle Types (method dispatch)
`List.*`, `Dict.*`, `Set.*`, `Queue.*`, `Table.*`, `StrBuf.*`, `Ptr.*`, `File.*`

### Layer 3: Imported Modules
`builtin/fs` → `fs.*`, `builtin/network` → `net.*`, `builtin/regex` → `re.*`, `builtin/crypto` → `crypto.*`, `builtin/data` → `data.*`, `builtin/binary` → `bin.*`, `builtin/websocket` → `ws.*`, `builtin/threads` → `thread.*`, `builtin/monitor` → `monitor.*`, `builtin/process` → `proc.*`, `builtin/debug` → `debug.*`, `builtin/performance` → `perf.*`, `builtin/system` → `sys.*` (deprecated), `builtin/math` → `math.*`

### Compiler Directives (attributes, no import)
`@inline`, `@noinline`, `@hot`, `@cold`

### Compiler Flags (CLI only)
`--lto`, `--strip`, `--minify`, `--gc-sections`
