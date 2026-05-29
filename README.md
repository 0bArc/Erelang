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

- **Interpreter runtime:** Actions, hooks, entities, globals, lists/dicts, plugin lifecycle.
- **Modular runtime:** Split across six translation units instead of one monolithic `runtime.cpp`.
- **Import-gated builtins:** Heavy I/O (network, fs, math, crypto, …) available only when a program imports the module.
- **Small core surface:** ~40 global builtins (time, env, args, strings, collections, plugin queries).
- **CMake build:** Static lib `erelang`, CLI `obc`, runner `erelang.exe` (from `erelang_runner`).

## Runtime layout

| File | Role |
|------|------|
| `src/runtime.cpp` | Program entry, `run`, `run_single_action`, plugin hook dispatch |
| `src/runtime_helpers.cpp` | Shared helpers, global list/dict/file handle state |
| `src/runtime_eval.cpp` | Expression evaluation, string interpolation |
| `src/runtime_actions.cpp` | Statement execution, action/hook/entity lookup |
| `src/runtime_builtins.cpp` | `eval_builtin_call` + import-gated module dispatch |
| `src/runtime_imports.cpp` | Import alias binding (`builtin/fs as fs` → `read_text`, …) |
| `src/builtins/*.cpp` | Optional module implementations (math, network, data, …) |

See [experimental/README.md](experimental/README.md) for the full import-gated module table.

### Core vs imported builtins

**Core (no import):** `now_ms`, `env`, `rand_int`, `uuid`, `args_*`, `read_line`, `input`, `exec`/`spawn`/`exit`, conversions, basic `string.*`, `list_*`, `dict_*`, `plugin_core*`, `language_name`/`language_version`.

**Import required:** `builtin/network`, `builtin/fs`, `builtin/math`, `builtin/data`, `builtin/perm`, `builtin/system`, `builtin/regex`, `builtin/crypto`, `builtin/binary`, and (with `-DERELANG_EXPERIMENTAL=ON`) `builtin/threads`, `builtin/monitor`.

**Removed from core:** machine identity helpers (`machine_guid`, `hwid`, `volume_serial`, `username`, `computer_name`), Win32 GUI builtins, low-level ptr/malloc, tables/sets/queues, duplicate file-handle APIs.

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

- `-DERELANG_EXPERIMENTAL=ON` — thread and monitor builtins
- `-DBUILD_SHARED_RUNTIME=OFF` — static runner only, no `erelang.dll`

Outputs (Debug):

- `build/bin/Debug/erelang.exe` — main runner (alias `obs.exe` on Windows)
- `build/bin/Debug/obc.exe` — compiler frontend
- `build/lib/Debug/liberelang.a` — static runtime library

### Run a program

```bash
./build/bin/Debug/erelang.exe examples/your_script.elan
```

Example with an imported module:

```elan
import builtin/math as math

public action main() {
  print(math.add(2, 3))
}
```

## Roadmap

- [x] Lexer, parser, AST
- [x] Typechecker with diagnostics
- [x] Interpreter runtime (modular split)
- [x] Import-gated builtin modules
- [x] Plugin manifest loading
- [ ] Shrink core builtins further; move fs helpers fully behind `builtin/fs`
- [ ] Advanced type system
- [ ] VM / LLVM backend

## Contributing

Open an issue or pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) if present.

## License

Apache-2.0. See [LICENSE](LICENSE).
