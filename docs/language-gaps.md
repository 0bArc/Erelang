# Erelang Language Gaps

Status overview of implemented vs. missing language features.  
[Y] = implemented  |  [~] = partial  |  [N] = missing

---

## Type System

| Feature | Status | Notes |
|---------|--------|-------|
| Primitive types (int, string, bool, double, pointer) | [Y] | |
| Type constructors `int(x)`, `string(x)`, `float(x)`, `bool(x)` | [Y] | v0.0.1+ |
| Type aliases (`str`->`string`, `char`->`string`) | [Y] | |
| Structs with typed fields | [Y] | |
| Entity/class with fields and methods | [Y] | |
| Enums with members | [Y] | |
| Generic collection types (`Array<T>`, `Map<K,V>`) | [Y] | |
| Optional types / `Option<T>` | [~] | API stubs only |
| Result types / `Result<T,E>` | [~] | API stubs only |
| Union types | [N] | |
| Real tagged Value type (not string-backed) | [N] | High priority |
| null safety / non-nullable types | [N] | |
| Type inference (`auto` / `var`) | [~] | Partial (needs `let`) |

---

## Collections

| Feature | Status | Notes |
|---------|--------|-------|
| Array/list literals `[1, 2, 3]` | [Y] | Native since v0.0.1 |
| Map/dict literals `{k: v}` | [Y] | |
| `for (v : array)` iteration | [Y] | |
| `for (k, v : map)` iteration | [Y] | |
| `list.*` methods (push, get, set, pop, etc.) | [Y] | Handle-based dispatch |
| `dict.*` methods (set, get, has, remove, etc.) | [Y] | Handle-based dispatch |
| Set collection | [~] | Stubs only (`set_new`, etc.) |
| Queue collection | [~] | Stubs only |
| Table/DataFrame collection | [~] | Stubs only |
| Destructuring assignment | [N] | |
| Range literals `0..100` | [N] | |
| Slices / views | [N] | |
| Typed collection methods (compile-time dispatch) | [N] | Runtime handle dispatch only |

---

## Control Flow

| Feature | Status | Notes |
|---------|--------|-------|
| `if`/`else` | [Y] | |
| `while` loop | [Y] | |
| `for (init; cond; step)` | [Y] | |
| `for (x : iterable)` for-each | [Y] | |
| `switch`/`case`/`default` | [Y] | String equality comparison |
| `break`/`continue` | [Y] | |
| `try`/`catch` | [Y] | |
| `return` | [Y] | |
| `do`/`while` | [Y] | |
| `repeat` (count loop) | [Y] | |
| `match` (pattern matching) | [~] | Falls through to `switch` |
| Pattern matching with destructuring | [N] | |
| Guard clauses in match arms | [N] | |

---

## Functions / Actions

| Feature | Status | Notes |
|---------|--------|-------|
| Named actions with typed params | [Y] | |
| Return type annotations (`action name(): int`) | [Y] | |
| `public`/`private`/`export` visibility | [Y] | |
| `extern action` declarations | [Y] | |
| Closures / lambdas | [N] | |
| First-class actions (actions as values) | [N] | |
| Currying / partial application | [N] | |
| Default parameter values | [N] | |
| Variadic parameters `...args` | [N] | |
| Named/keyword arguments | [N] | |
| Overloaded actions (same name, diff params) | [N] | |
| Generator / iterator actions (`yield`) | [N] | |

---

## Module System

| Feature | Status | Notes |
|---------|--------|-------|
| `#include <builtin/module> as alias` | [Y] | |
| `import "path" as alias` | [Y] | |
| Named imports `import {a, b} from "path"` | [Y] | |
| Plugin system (`.elp` manifests) | [Y] | |
| Builtin module resolution (`builtin/*`) | [Y] | |
| Circular import detection | [N] | |
| Module re-export / `pub use` | [N] | |
| Version pinning in imports | [N] | |
| Package manager / registry | [N] | |

---

## Modular Builtins

| Module | Import Path | Status | Methods |
|--------|------------|--------|---------|
| Filesystem | `builtin/fs` | [Y] | read, write, append, exists, is_dir, is_file, mkdir, copy, move, remove, list, dirs, files, size, mtime, cwd, chdir |
| Path | `builtin/path` | [Y] | join, parent, dirname, name, basename, ext, exists |
| Network | `builtin/network` | [Y] | get, post, put, patch, delete, download, encode, status |
| WebSocket | `builtin/websocket` | [Y] | connect, send, recv, close |
| Regex | `builtin/regex` | [Y] | match, find, find_all, replace, split, capture, group, compile, free, test |
| Crypto | `builtin/crypto` | [Y] | hash (FNV-1a), random_bytes |
| Math | `builtin/math` | [Y] | add, sub, mul, div, mod, min, max, abs, sin, cos, tan, sqrt, pow |
| Threads | `builtin/threads` | [Y] | spawn, sleep, join, active, gc, kill |
| Monitor | `builtin/monitor` | [~] | Experimental (ERELANG_EXPERIMENTAL) |
| Data | `builtin/data` | [Y] | new, set, get, has, keys, save, load |
| Binary | `builtin/binary` | [Y] | new, from_hex, to_hex, len, push_u8, get_u8 |
| Permissions | `builtin/perm` | [Y] | grant, revoke, has, list |
| Process | `builtin/process` | [Y] | shell, execute, spawn, output, exit_code |
| Performance | `builtin/performance` | [Y] | profile.begin, profile.end, mem.usage, mem.peak, gc.collect |

---

## Missing Builtin APIs

| Module | Missing | Priority |
|--------|---------|----------|
| `builtin/fs` | glob, walk, copy_dir, temp_file, temp_dir, watch | Medium |
| `builtin/net` | JSON body helpers, timeout config, redirect control | Medium |
| `builtin/math` | Float operations (sinf, cosf), log, exp, round, floor, ceil | Medium |
| `builtin/regex` | Case-insensitive flag, multiline flag, Unicode support | Medium |
| `builtin/process` | Environment vars, working dir, timeout | Medium |
| General | format/printf, REPL, debugger integration | High |

---

## Language Server (VSCode Extension)

| Feature | Status | Notes |
|---------|--------|-------|
| Syntax highlighting (TextMate) | [Y] | All keywords and builtins |
| Code completion (IntelliSense) | [Y] | Keywords, symbols, modules, dot-access |
| Semicolon diagnostics | [Y] | |
| Document symbols | [Y] | Entities, actions, fields, hooks |
| Workspace symbols | [Y] | Cross-file |
| Code snippets | [Y] | Actions, entities, arrays, dicts, for-each |
| Include path auto-complete | [Y] | File system browsing + builtin list |
| Go to definition | [N] | |
| Find references | [N] | |
| Hover information | [N] | |
| Signature help | [N] | |
| Code formatting | [N] | |
| Rename symbol | [N] | |
| LSP-based language server | [N] | Currently VSCode-native only |

---

## Compiler / Build

| Feature | Status | Notes |
|---------|--------|-------|
| Interpreter mode (run `.elan` directly) | [Y] | |
| AOT compile to `.exe` | [Y] | Via embedded CMake + MinGW |
| `--lto` link-time optimization | [Y] | |
| `--strip` debug symbols | [Y] | |
| `--minify` embedded sources | [Y] | |
| `--gc-sections` dead code elimination | [Y] | |
| IR emission (`--emit-ir`) | [Y] | |
| ASM emission (`--emit-asm`) | [~] | Stub output |
| Hot reload / `--watch` | [N] | |
| Self-hosting compiler | [N] | |
| Reproducible builds | [N] | |
| Cross-compilation targets | [N] | Windows only |
| Embedded WASM target | [N] | |

---

## Testing / Debugging

| Feature | Status | Notes |
|---------|--------|-------|
| `print` diagnostics | [Y] | |
| Debug module (`builtin/debug`) | [Y] | Breakpoints, trace |
| `--debug` flag loader | [Y] | Via `lib/debugger.elan` |
| Built-in test blocks | [N] | |
| CTest / golden-file harness | [N] | |
| Assertion builtins | [N] | Only `is_base_of`, `dynamic_cast` |
| Stack traces on error | [N] | |
| Profiler output to file | [~] | Console only |

---

## Performance / Size

| Metric | Value | Notes |
|--------|-------|-------|
| Compiled binary | ~1-2 MB | With `--lto --strip --gc-sections` |
| Parse + run overhead | Minimal | String-backed values are the bottleneck |
| Thread model | Cooperative | Experimental thread pool |

---

## Suggested Priority Order

1. **Real Value type** — foundation for type safety, perf, and ergonomics  
2. **Closures / lambdas** — unlocks functional patterns  
3. **LSP-based language server** — reliable cross-editor support  
4. **CTest harness** — regression prevention  
5. **REPL** — rapid prototyping  
6. **Format/printf** — common request  
7. **Package manager** — ecosystem growth  
