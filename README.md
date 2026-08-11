# Erelang (Erevos Language)

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Build](https://img.shields.io/badge/build-CMake-orange)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![GitHub Stars](https://img.shields.io/github/stars/0bArc/Erelang)
![GitHub Forks](https://img.shields.io/github/forks/0bArc/Erelang)
![GitHub Issues](https://img.shields.io/github/issues/0bArc/Erelang)
![GitHub Last Commit](https://img.shields.io/github/last-commit/0bArc/Erelang)
![Repo Size](https://img.shields.io/github/repo-size/0bArc/Erelang)
![License](https://img.shields.io/github/license/0bArc/Erelang)

---

## Overview

**Erelang** (Erevos Language) is a scripting language and interpreter toolchain built in **C++20** with CMake. It provides a lexer, parser, typechecker, optimizer, and runtime for `.elan` programs, plus optional plugin and module loading on Windows.

## Features

- **Modular runtime:** Actions, hooks, entities, globals, lists, dicts, and plugin lifecycle across focused translation units under `src/runtime/`.
- **Import-gated modules:** Network, filesystem, math, crypto, threads, websocket, and more are only linked when a program imports them. See [docs/](docs/README.md) for the full list.
- **Semantic builtin API:** Organized by ownership. `fs.read()`, `net.get()`, `thread.spawn()`, `proc.execute()`, `data.set()`, `bin.push_u8()`. Type constructors like `int()`, `float()`, `string()`, `bool()` are always available without import.
- **Debug and performance:** Debug module with leveled logging, assertions, timers, and breakpoints (`debug.log`, `debug.assert`, `debug.timer_start`). Performance profiling API (`perf.profile.begin`, `perf.mem.usage`, `perf.gc.collect`).
- **Compiler directives:** `@inline`, `@noinline`, `@hot`, `@cold` for fine-grained optimization control.
- **Compile flags:** `erelang --compile app.elan --strip --minify --lto --gc-sections` for sub-1MB static executables.
- **Strict type checking:** Type mismatches are compile-time hard errors. `any` is opt-in only; no silent type fallback.
- **CMake build:** Static lib `erelang`, CLI `obc`, runner `erelang.exe` (from `erelang_runner`).

## Runtime layout

All runtime code lives under `src/runtime/`.

| File | Role |
|------|------|
| `src/runtime/core.cpp` | Program entry, `run`, `run_single_action`, plugin hook dispatch |
| `src/runtime/helpers.cpp` | Shared helpers, global container state (list/dict/file/set/queue) |
| `src/runtime/eval.cpp` | Expression evaluation, string interpolation |
| `src/runtime/actions.cpp` | Statement execution, handle-type method dispatch |
| `src/runtime/builtins.cpp` | `eval_builtin_call`, type constructors, debug module, import-gated dispatch |
| `src/runtime/imports.cpp` | Import alias binding (`builtin/fs as fs` maps `fs.read` to `read_text`) |
| `src/runtime/builtins/*.cpp` | Module implementations: math, network, data, regex, crypto, binary, system, websocket, performance |
| `src/runtime/features/*.cpp` | Runtime language features (serialization) |

### Core vs imported builtins

**Core (no import, always available):**

`print`, `time.now_ms`, `time.now_iso`, `time.uuid`, `time.rand_int`, `env.get`, `env.load_dotenv`, `int()`, `float()`, `string()`, `bool()`, `int.is()`, `float.is()`, `string.is()`, `char.is_digit`, `char.is_space`, `char.is_alpha`, `lang.name`, `lang.version`, `lang.about`, `lang.limitations`, `json.encode`, `json.decode`, `plugin.core`, `plugin.core_files`, `plugin.core_keys`, `io.read_line`, `io.stdin`, `io.stderr`, `io.prompt`, `io.input`, `color.red`, `color.green`, `color.yellow`, `color.blue`, `color.magenta`, `color.cyan`, `color.bold`, `color.reset`, `list.*`, `dict.*`, `set.*`, `queue.*`, `file.*`, `ptr.*`, `strbuf.*`

**Import required:**

| Module | Import path | Example alias |
|--------|------------|---------------|
| Filesystem | `builtin/fs` | `fs.read`, `fs.write`, `fs.exists` |
| Path utilities | `builtin/path` | `path.join`, `path.parent`, `path.name` |
| Network | `builtin/network` | `net.get`, `net.post`, `net.download` |
| Math | `builtin/math` | `math.sin`, `math.sqrt`, `math.abs` |
| Data | `builtin/data` | `data.new`, `data.set`, `data.get` |
| Regex | `builtin/regex` | `re.match`, `re.find`, `re.replace` |
| Crypto | `builtin/crypto` | `crypto.hash`, `crypto.random_bytes` |
| Binary | `builtin/binary` | `bin.new`, `bin.push_u8`, `bin.get_u8` |
| Process | `builtin/process` | `proc.execute`, `proc.output`, `proc.exit_code` |
| System (deprecated) | `builtin/system` | `system.execute`, `system.cmd` |
| WebSocket | `builtin/websocket` | `ws.connect`, `ws.send`, `ws.recv` |
| Permissions | `builtin/perm` | `perm.grant`, `perm.revoke` |
| Debug | `builtin/debug` | `debug.log`, `debug.assert`, `debug.timer_start` |
| Performance | `builtin/performance` | `perf.profile.begin`, `perf.mem.usage` |
| Threads* | `builtin/threads` | `thread.spawn`, `thread.join`, `thread.sleep` |
| Monitor* | `builtin/monitor` | `monitor.add`, `monitor.list` |

\* Requires `-DERELANG_EXPERIMENTAL=ON` at build time.

## Getting Started

### Prerequisites

- **Compiler:** MSVC 2019+, MinGW, or Clang with C++20 support
- **CMake:** 3.15+

### Build

```bash
git clone https://github.com/0bArc/Erelang.git
cd Erelang

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target erelang_runner -j 8
```

Optional flags:

- `-DERELANG_EXPERIMENTAL=ON`: thread and monitor builtins
- `-DBUILD_SHARED_RUNTIME=OFF`: static runner only, no `erelang.dll`

Outputs (Debug):

- `build/bin/Debug/erelang.exe`: main runner (alias `obs.exe` on Windows)
- `build/bin/Debug/obc.exe`: compiler frontend
- `build/lib/Debug/liberelang.a`: static runtime library

### Run a program

```bash
./build/bin/Debug/erelang.exe examples/program.elan
```

Example with imported modules:

```elan
@erelang
#include <builtin/math> as math
#include <builtin/debug> as debug

public action main {
    int result = math.add(2, 3);
    debug.log("computed: " + string(result));
}

run main;
```

## Documentation

Full docs live under **[docs/](docs/README.md)**. Topic guides cover each builtin module (`filesystem.md`, `network.md`, `process.md`, `regex.md`, `threads.md`, etc.) plus language fundamentals (`language.md`, `imports.md`, `diagnostics.md`).

## Status

**Alpha.** All doc examples validated and passing as of 2026-08-11.

Tested modules: core builtins, collections, strings, math, crypto, regex, binary, data, filesystem, process, debug, performance, websocket, and automation patterns.

## Roadmap

Active development is focused on closing language gaps and improving consistency. See [`docs/language-gaps.md`] for the current list of identified areas being worked toward. Updates and documentation are provided as improvements stabilize.

## Contributing

Open an issue or pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) if present.

## License

Apache-2.0. See [LICENSE](LICENSE).
