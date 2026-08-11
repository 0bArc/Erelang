# 2026-05-29  --  Runtime modularization & builtin trim

## Summary

Split the ~3.8k-line `runtime.cpp` monolith into six focused translation units, gated most non-core builtins behind `import builtin/...`, and trimmed the global builtin table from ~150+ entries to ~40. Removed stale build directories and machine-identity helpers.

## Runtime split

Previously: one `runtime.cpp` held init, eval, actions, and all builtin dispatch.

Now:

| TU | ~LOC | Responsibility |
|----|------|----------------|
| `runtime.cpp` | 450 | `Runtime` ctor/init, `run`, `run_single_action`, plugin hook orchestration |
| `runtime_helpers.cpp` | 290 | Shared helpers, global container state (`g_lists`, `g_dicts`, …) |
| `runtime_eval.cpp` | 400 | `eval_string`, interpolation expr cache |
| `runtime_actions.cpp` | 940 | `exec_stmt`, `exec_block`, `find_action` / hook / entity |
| `runtime_builtins.cpp` | 1900 | `eval_builtin_call`, import module dispatch |
| `runtime_imports.cpp` | 245 | `program_imports_module`, alias binding |

New header: `include/erelang/runtime_helpers.hpp`  --  shared runtime internals used across TUs.

Mechanical split script: `tmp/split_runtime.py`.

## Builtin policy change

### Core globals (~40)

Typechecker `init_builtins()` now registers only essentials: time/env/args, process I/O, conversions, basic `string.*`, `list_*`/`dict_*`, plugin core queries, language metadata.

### Import-gated modules

Runtime and typechecker only expose these when the program imports the path:

- `builtin/network`, `builtin/regex`, `builtin/crypto`, `builtin/binary`
- `builtin/math` (incl. collatz helpers)
- `builtin/data`, `builtin/perm`, `builtin/system`
- `builtin/fs` / `builtin/path` (filesystem helpers moved off global table)
- `builtin/threads`, `builtin/monitor` (requires `-DERELANG_EXPERIMENTAL=ON`)

`runtime_builtins.cpp` calls `__erelang_builtin_*_dispatch` only when `program_imports_module()` matches. `TypeChecker::register_imported_module_builtins()` adds signatures per import.

### Removed / dropped from core

- Machine identity: `machine_guid`, `hwid`, `volume_serial`, `username`, `computer_name`
- Win32 GUI / window entity builtins (already stripped earlier)
- Low-level memory: `ptr_*`, `malloc`, `make_unique`, …
- Extra collections: `set_*`, `queue_*`, `table_*`
- Duplicates: `fopen`/`file_open`, `string_buffer_*`/`strbuf_*`, `os.*` aliases
- Verbose meta: `language_about`, `language_limitations`, `color.*`
- `option_*`, `result_*` helpers

`uuid` kept in core (uses RPC on Windows).

## Typechecker

- `init_builtins()`  --  slim core table
- `register_imported_module_builtins(program)`  --  per-program additions when imports seen
- `resolve_builtin_module_alias_call()`  --  `fs.read` → `read_text` style resolution (unchanged pattern, extended for math/data/perm/system)

## Build & repo hygiene

- `CMakeLists.txt`  --  added `runtime_helpers.cpp`, `runtime_eval.cpp`, `runtime_actions.cpp` to `erelang` / `erelang_shared` targets
- Deleted local cruft: `build_p1/`, `build_fix/`, `build_tmp/`
- `.gitignore`  --  ignore `build_p1/`, `build_tmp/`, `.cache/`
- `experimental/README.md`  --  module table updated

## Verify

```bash
cmake --build build --target erelang_runner -j 8
# → build/bin/Debug/erelang.exe
```

## Follow-ups

- Move remaining fs builtins fully behind `import builtin/fs` in runtime dispatch (typechecker already gates signatures)
- Further shrink `runtime_builtins.cpp` by moving inline handlers into `src/builtins/*.cpp`
- Update VS Code extension keyword lists (`machine_guid`, etc.) to match trimmed core
