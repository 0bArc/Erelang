# Language gaps

A consolidated, status-annotated list of features Erelang is missing, built
from `implement.md`, `docs/`, and direct inspection of the parser
(`src/parser.cpp`), lexer (`src/lexer.cpp`), runtime (`src/runtime/`),
builtins (`src/runtime/builtins/`), and toolchain (`src/obs_main.cpp`,
`src/main.cpp`).

Note: the plan referenced `dev/missing.md` as a source; that file does not
exist in the repo, so `implement.md` plus code inspection were used instead.

Status legend (verified against the code at the time of writing):

| Marker | Meaning |
|--------|---------|
| Yes | Implemented (found in the code) |
| Partial | Partial: syntax/parse exists, but semantics are incomplete or string-backed |
| No | Missing |

---

## Type system

Everything is still a string at runtime. This is the root cause of the
truthiness / numeric / comparison inconsistencies documented in
`runtime-review.md`.

| Feature | Status | Notes |
|---------|--------|-------|
| Real `bool` / `int` / `float` / `null` types | No | Values are `std::string`; `true`/`false` are string literals |
| Type inference (`x = 10` -> int) | Partial | Typed declarations parse, but inferred values stay strings |
| Type annotations (`int x = 10;`) | Yes | `parser.cpp` typed declarations (`parse_type_annotation`) |
| Null / none type (`string? x = none;`) | No | No distinct missing-value type |
| Generic type annotations (`List<int>`, `Dict<string,string>`) | Yes | Parser handles `>>`/`<<` nesting (`parser.cpp:2258-2271`) |

## Collections (typed method syntax)

The old `list_get(list, i)` / `dict_has(dict, k)` handle API has largely been
replaced by typed collections with method access.

| Feature | Status | Notes |
|---------|--------|-------|
| `List<int>()` / `Dict<string,string>()` construction | Yes | Typed declaration + type-aware runtime (`parse_runtime_array_type`) |
| List methods `add/get/remove/clear/contains/join/slice/sort/reverse/len` | Yes | Dynamic list method dispatch (`src/runtime/actions.cpp:585-598`, `729`) |
| Dict methods `set/get/has/remove/keys/values/clear/size` | Yes | Dynamic dict/map method dispatch (`actions.cpp:598-600`) |
| Index read `nums[0]` | Yes | `IndexExpr` in postfix parsing (`parser.cpp:417-420`) |
| Index write `config["host"] = "x"` | No | Only reads are lowered; indexed assignment not implemented |
| `from_json` returning typed `Dict`/`List` | Partial | JSON round-trips through string-typed containers |

## Action return values

| Feature | Status | Notes |
|---------|--------|-------|
| `-> type` return type annotation | Yes | `parser.cpp:880-881` |
| Return value propagation | Yes | `ctx.returnValue` (`actions.cpp:239-246`, `eval.cpp:490`, `builtins.cpp:129-153`) |
| Typechecker return-type validation | Yes | `TC121` missing-return diagnostic (`typechecker.cpp:901-921`) |

## Strings

| Feature | Status | Notes |
|---------|--------|-------|
| `string.lstrip/rstrip/strip/lower/upper` | Yes | `builtins.cpp:737-758` |
| `string.starts_with/ends_with/find/substr/len` | Yes | `builtins.cpp:768-799` |
| `string.split(s, sep)` | No | No split builtin |
| `string.contains(s, sub)` | Partial | Only as a list/map method, not a string helper |
| `string.replace(s, old, new)` | No | |
| `string.repeat(s, n)` | No | |
| `string.pad_left/right`, `trim_prefix/suffix` | No | |
| `char_at`, `ord`, `chr` | No | |
| Multi-line string literals (`"""..."""` / backtick) | No | Raw strings exist but no triple-quote block strings |
| Unicode escape `\uXXXX` in literals | Yes | Fixed to decode real UTF-8 (`lexer.cpp`) |
| Duration literals (`2m30s`) | Yes | `Duration` token; now word-boundary aware |

## Operators and expressions

| Feature | Status | Notes |
|---------|--------|-------|
| Ternary `cond ? a : b` | Yes | `parse_ternary` (`parser.cpp:309`) |
| Null coalescing `??` | Yes | `parser.cpp:325` |
| Compound assignment `+=`, `-=`, `*=`, etc. | No | Tokens are lexed (`PlusAssign`...) but the parser never handles them: a parse error today |
| Range literals `0..10` / `0..=10` | No | No `..` token |
| Pipe operator `x |> f |> g` | No | |
| Logical `&&` / `\|\|` / `!` | Yes | String-backed semantics (`runtime-review.md` P0-16) |
| Hex integer literals `0xFF` | Yes | Fixed `0x1e`-style parse bug (`parser.cpp:172-214`) |
| Bitwise operators `& \| ^ ~ << >> >>>` | Yes | Tokens + `Shr`/`Shl` handling |
| `**` power operator | Yes | `Pow` token; optimizer cap on exponent (DoS guard) |

## Control flow

| Feature | Status | Notes |
|---------|--------|-------|
| `for (i in 0..10)` numeric range loop | No | Requires range literals |
| `for (i, item : list)` indexed iteration | Yes | `parser.cpp:1796` |
| `for (x : items)` / `for (x in items)` | Yes | `parser.cpp:1803` |
| `for (auto& x : items)`, `for (int *p : items)` | Yes | Reference/pointer qualifiers (`parser.cpp:1784-1787`) |
| `while`, `do/while`, `repeat` | Yes | `parser.cpp:1667-1683`, `1059` |
| `loop { }` infinite loop | No | Use `while true` instead |
| Pattern-matching `switch` | No | `switch` only string-equality (`actions.cpp`, documented in `runtime-review.md`) |
| `break` / `continue` on native path | Yes | IR break/continue opcodes now emitted (`ir.cpp`) |
| Enum associated values (`Status.Err("msg")`) | No | Bare-name enums only |

## Error handling

| Feature | Status | Notes |
|---------|--------|-------|
| `try { } catch (e) { }` | Yes | `TryCatchStmt` (`parser.cpp:2100`) |
| `assert(cond, msg)` | No | No assert builtin |
| Action return on failure / recovery | No | Runtime errors propagate as exceptions, no `or exit(n)` recovery |
| Stack trace with line numbers on runtime error | No | Syntax/typecheck diagnostics have locations; runtime errors do not |

## Closures and first-class actions

| Feature | Status | Notes |
|---------|--------|-------|
| Actions as values / inline lambdas | No | No `LambdaExpr`; actions are declarations only |
| `List.filter/map/reduce(fn)` | No | Requires closures |

## Destructuring

| Feature | Status | Notes |
|---------|--------|-------|
| `[a, b, c] = [10, 20, 30]` | No | |
| `{host, port} = {...}` | No | |
| Swap without temp | No | |

## Variadic actions

| Feature | Status | Notes |
|---------|--------|-------|
| `action log(...args)` | No | No ellipsis param support |

## Enums

| Feature | Status | Notes |
|---------|--------|-------|
| Bare enums + `switch` on members | Yes | `examples/enum_switch_loop.elan` |
| Associated values (`Err(string)`, `Pending(int)`) | No | |
| Enum methods | No | |

## Filesystem

| Feature | Status | Notes |
|---------|--------|-------|
| `file_open/read/write/seek/tell/flush/exists/size/mtime/close` | Yes | `builtins.cpp:1007-1168` |
| `path_join/dirname/basename/ext`, `read_text/write_text/read_line` | Yes | `builtins.cpp:931-1168` |
| `fs.glob(dir, pattern)` | No | |
| `fs.walk(dir)` recursive | No | |
| `fs.is_dir(path)` | No | |
| `fs.copy_dir(src, dst)` | No | |
| `fs.remove_dir(path)` recursive | No | |
| `fs.temp_file()` / `fs.temp_dir()` | No | |

## Network

| Feature | Status | Notes |
|---------|--------|-------|
| `http_get / http_post / http_status / http_download` | Yes | `network.cpp:408-418` |
| `hls_download_best`, `url_encode`, `network.ip.*` | Yes | `network.cpp:416-435` |
| `net.headers(url)` | No | |
| `net.post_json / get_json` | No | |
| `net.put` / `net.delete` | No | |
| HTTP timeout / response cap | Yes | `kMaxResponseBytes`, pipe/read timeouts |
| JSON encode/decode | Partial | Feature-level (`features/serialization.cpp`) works; no `net.*_json` wrappers |

## Process / shell

| Feature | Status | Notes |
|---------|--------|-------|
| `exec` / `os.exec` / `spawn` / `os.spawn` | Yes | `builtins.cpp:834-909` (shell-based by design) |
| `system.execute(args...)` capturing output | Yes | Argument-array API with quoting fix (`builtins/system.cpp`) |
| `exec_capture(cmd)` as a core builtin | No | Use `system.execute` |
| `env_set(key, value)` | No | |
| `env_all()` | No | |
| `which(name)` | No | |
| CLI args readable as `string` via `args_get(i)` | Partial | `args_get` returns a string, but the runtime re-infers a numeric-looking value (e.g. `"2"`) as `int`, so `string a = args_get(0)` fails with a type mismatch for numeric args. Pass args straight into a consuming builtin (e.g. `math.add(args_get(0), args_get(1))`) instead of storing them in a typed `string` variable. See `testfile.elan` for a working pattern. |

## Math

| Feature | Status | Notes |
|---------|--------|-------|
| `add/sub/mul/div/mod/min/max/abs` | Yes | `math.cpp:34-60`, checked against overflow |
| `sin/cos/tan/sqrt/pow` | Yes | `math.cpp:49-53` |
| `math.floor/ceil/round` | No | |
| `math.log/log2/log10` | No | |
| `math.pi` / `math.e` constants | No | |
| Float arithmetic that preserves floats | Partial | Values are strings; `Neg` etc. now preserve floats, but general float math is string-backed |

## Builtins (small gaps)

| Feature | Status | Notes |
|---------|--------|-------|
| `sleep(ms)` as a function call | Partial | Only `sleep Nms` statement syntax (`parser.cpp:1118`); hex/float durations fixed |
| `now_unix()` | No | |
| `format(template, ...args)` | No | |
| `printf` / `print_err` | No | `stderr_print` exists but is verbose |
| `parse_int(s, base?)` | No | |
| Core `min`/`max` without importing `math` | No | Only under `builtin/math` |
| `read_line` / `read_text` / `write_text` | Yes | `builtins.cpp:931-995` |

## Module system

| Feature | Status | Notes |
|---------|--------|-------|
| `#include` + local modules | Yes | `docs/imports.md`; `runtime/imports.cpp` |
| Named imports without alias (`use builtin/fs; fs.read(...)`) | Yes | `resolve_builtin_module_method` (`actions.cpp:550`) |
| Module-level `const` | Yes | `parser.cpp:1136` |
| Re-export from another module | No | |
| Circular import detection | No | Currently silent |
| Version pinning (`#include <mylib@1.2>`) | No | |

## Formatting / output

| Feature | Status | Notes |
|---------|--------|-------|
| `print` / `println` interpolation | Yes | Core output |
| Color output as core API | No | Color handling is buried in `obs_main.cpp` |
| `print_table(list_of_dicts)` | No | |

## Testing

| Feature | Status | Notes |
|---------|--------|-------|
| `test "name" { ... }` blocks | No | |
| `erelang test file.elan` | No | |
| `assert(cond)` / `assert_eq(a, b)` helpers | No | |

## Tooling

| Feature | Status | Notes |
|---------|--------|-------|
| REPL (`erelang repl`) | No | |
| `--watch` re-run on file change | No | |
| Stack trace on runtime error | No | See Error handling |
| `erelang fmt` formatter | No | |
| LSP / language server | No | |
| Check-only mode | Partial | `checkfile.elan` runs typecheck but still demands a `run` target (`TC111`) |
| `--emit-asm` | Yes | Now emits real GAS x64 assembly (`main.cpp`) |

## Self-hosting blockers

- **Native codegen is demo-level**: the x64 backend (`src/codegen_x64.cpp`)
  emits GAS assembly for Windows (`emit_gas_win64_demo`); there is no
  end-to-end compile-to-PE pipeline, no linker integration, and no
  self-hosted standard library.
- **No PE emission / reproducible builds**: no deterministic binary output,
  so a self-hosted `erelang` cannot bootstrap itself yet.
- **Runtime is string-backed**: self-hosting a type-safe standard library is
  impractical until the `Value` type is a real tagged union (see
  `runtime-review.md` suggestions).
- **No interpreter-in-interpreter test**: there is no test that runs the
  compiler itself through `erelang`/`obc`, which would be the first step
  toward self-hosting.

---

## Additional gaps (new)

Below are features not yet tracked in the lists above, organized by area.

### Date and time

| Feature | Status | Notes |
|---------|--------|-------|
| `now_unix()` | No | Unix timestamp as integer |
| Date/time module (`builtin/datetime`) | No | Parse, format, diff, add/subtract days; `datetime.parse("2026-01-15")`, `datetime.format(ts, "%Y-%m-%d")`, `datetime.diff(d1, d2, "days")` |
| `sleep(ms)` as a function call | Partial | Only `sleep Nms` statement syntax; function form needed for callbacks |

### Encoding and data formats

| Feature | Status | Notes |
|---------|--------|-------|
| Base64 encode/decode | No | `base64_encode(data)`, `base64_decode(text)` -- essential for binary payloads in scripts |
| CSV/TSV parsing | No | `csv_parse(text, delimiter?)` returns list of dicts; first row as headers |
| INI config parsing | No | `ini_parse(text)` returns dict of sections -> dict of key/values |
| TOML config parsing | No | `toml_parse(text)` returns nested dict -- standard config format |
| URL query string parsing | No | `url_parse_query("a=1&b=2")` returns dict; complement to `net.encode` |

### Type system

| Feature | Status | Notes |
|---------|--------|-------|
| Type aliases (`typealias`) | No | `typealias UserId = string;` -- improves readability without runtime cost |
| Optional / nullable types | No | `string? name = none;` -- distinct from empty string; unblocks safe JSON/DB access |
| Sum types / tagged unions | No | `type Result = Ok(string) \| Err(string, int);` -- needed for error handling beyond exceptions |

### Collections and iteration

| Feature | Status | Notes |
|---------|--------|-------|
| Set data structure | No | `set_new()`, `set_add(s, v)`, `set_has(s, v)`, `set_remove(s, v)`, `set_union(a, b)`, `set_intersect(a, b)` -- unique value collections |
| Sorted / ordered dictionary | No | `odict_new()` preserving insertion order; `dict_sort(handle, by_key\|by_value)` -- predictable iteration |
| `list_find(list, value)` | No | Index of first match, or -1; currently requires manual loop |
| `list_sort(list, asc?)` | Partial | Sort is mentioned as a list method but not yet in the runtime dispatch |

### Console and UI

| Feature | Status | Notes |
|---------|--------|-------|
| Color output API | No | `color_fg(r, g, b)`, `color_reset()` -- ANSI/VT escape helpers; color handling is currently buried in `obs_main.cpp` |
| Progress bar / spinner | No | `progress_start(n)`, `progress_tick()`, `progress_done()` -- console progress for long-running tasks |
| System notifications | No | Windows toast / macOS notification center; `notify(title, body)` for alerting users |
| Clipboard access | No | `clipboard_read()`, `clipboard_write(text)` -- common automation need |
| `read_key()` / `key_pressed()` | No | Single-key input without line buffering; interactive menus |

### Data transformation

| Feature | Status | Notes |
|---------|--------|-------|
| Pipe operator | No | `x \|> trim \|> parse \|> validate` -- transforms data through a chain of actions |
| Map/filter/reduce on lists | No | `list_map(list, fn)`, `list_filter(list, fn)`, `list_reduce(list, fn, init)` -- requires closures, blocked by no lambda support |
| `string.join(list, sep)` | No | Opposite of split; `["a","b","c"].join(",")` -> `"a,b,c"` |

### Networking

| Feature | Status | Notes |
|---------|--------|-------|
| WebSocket client | Yes | `ws_connect(url)` returns handle; `ws_send(h, msg)`, `ws_recv(h)`, `ws_close(h)` -- real-time communication |
| `net.get_auth(url, auth)` / `net.post_auth(url, body, ct, auth)` | Yes | Authenticated HTTP with custom `Authorization` header |
| `net.put(url, body)` / `net.delete(url)` | No | Missing HTTP verbs beyond GET/POST |
| `net.head(url)` | No | Fetch response headers without body; returns dict of header key/value pairs |
| SMTP email sending | No | `mail_send(server, port, from, to, subject, body)` -- notification delivery for automation |

### Process and system

| Feature | Status | Notes |
|---------|--------|-------|
| Process piping | No | `sys.pipe("cmd1 \| cmd2")` or `sys.pipe(["cmd1", "cmd2"])` -- chain commands; currently requires temp files |
| `env_set(key, value)` / `env_all()` | No | Mutate environment in-process; needed for toolchain wrappers and CI scripts |
| `which(name)` / `find_executable(name)` | No | Locate a binary on PATH; useful before calling `exec` |

### Compression

| Feature | Status | Notes |
|---------|--------|-------|
| gzip compress/decompress | No | `gzip_compress(data)`, `gzip_decompress(data)` -- binary data for file transfer |
| zlib compress/decompress | No | `zlib_compress(data)`, `zlib_decompress(data)` -- memory-level compression |

### Logging

| Feature | Status | Notes |
|---------|--------|-------|
| Structured logging | No | `log_info(msg)`, `log_warn(msg)`, `log_error(msg)` with timestamp, level, file/line; redirectable to file or stderr |
| Log rotation | No | `log_set_file(path, maxBytes?, maxFiles?)` -- auto-rotate log files |

### Tooling and developer experience

| Feature | Status | Notes |
|---------|--------|-------|
| Hot reload / file watcher | No | `--watch` flag: re-run script when source or dependencies change; pairs with `builtin/monitor` |
| Benchmarking builtins | No | `time(action_name)` -- runs an action N times, reports min/mean/max; `bench_start(label)`, `bench_end(label)` |
| REPL history and tab completion | No | Currently no REPL at all; when built, needs line editing, history, and action/entity name completion |
| Package registry / manifest | No | Central index of `.elan` libraries; `erelang install <pkg>`; versioned dependencies in project manifests |

### Security and sandboxing

| Feature | Status | Notes |
|---------|--------|-------|
| `--sandbox` mode | No | Disable network, filesystem, and process builtins; whitelist-approved paths/modules only |
| Permission scoping | No | `perm_scope("network") { ... }` -- grant permission only within a block; auto-revoke on exit |
| Script signing | No | Cryptographic signature verification before execution; `erelang --verify-signature script.elan` |

### Documentation and help

| Feature | Status | Notes |
|---------|--------|-------|
| `help(action_name)` builtin | No | Print action signature and doc comment at runtime; `help("fs.read")` shows args and returns |
| Doc comment extraction | No | `///` or `##` comments extracted by `erelang docs` into markdown; self-documenting libraries |

## Suggested priorities

1. **Real `Value` type** (bool/int/float/null): unblocks most of the
   string-backed semantic debt above.
2. **Compound assignment**: tokens already exist; parser support is a small,
   high-value addition.
3. **Range literals + `for (i in 0..10)`**: removes the most common
   boilerplate loop pattern.
4. **`assert` + `test` blocks**: enables the CTest golden harness proposed in
   `runtime-review.md`.
5. **`string.split` / `format`**: two most-requested string gaps for
   automation scripts.
6. **Date/time module**: date arithmetic and formatting is a constant need
   in automation scripts (log rotation, report generation, scheduling).
7. **Base64 encoding**: critical for scripts that handle binary payloads
   (attachments, API tokens, data serialization).
8. **CSV/JSON round-trip helpers**: parse CSV to dicts, convert between
   formats; Erelang already has JSON -- close the transformation gap.
9. **`--watch` / hot reload**: makes the development loop dramatically
   faster for script-heavy workflows.
10. **Structured logging with file rotation**: every long-running automation
    needs logs; `print` does not scale.
11. ~~WebSocket client~~ (IMPLEMENTED): `ws_connect`, `ws_recv`, `ws_close` available via `#include <builtin/websocket> as ws`.
12. WebSocket client: real-time communication unlocks chat bots,
    notification relays, and live dashboards in pure Erelang.
12. **Color output + progress bar**: console-first scripts need visual
    feedback beyond plain `print`.
13. **`env_set` / `env_all`**: toolchain wrappers and CI scripts frequently
    need to mutate environment variables.
14. **Type aliases and optional types**: improve code clarity with zero
    runtime cost; optional types eliminate "empty string means missing"
    ambiguity.
15. **Set collections and `list_find`**: fills collection API gaps that
    currently require manual loops.
