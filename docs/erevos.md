# Erelang Project Overview

This document walks every major asset in the repository so you can see how the language, runtime, tooling, and shipping assets fit together. It references each source file and directory, summarises the features implemented today, and highlights the remaining gaps that show up in the implementation manifest.

## Repository tour

### Top-level items

| Path | Role |
| --- | --- |
| `CMakeLists.txt` | Root build script for all binaries (runtime, CLI tools, DLLs, native GUI helpers). |
| `BOILERPLATE.md` | Scratchpad for release notes and boilerplate legal text. |
| `README.md` | High-level elevator pitch plus roadmap bullets for the Erelang syntax. |
| `implemented.manifest` | Living checklist of implemented language features, built-ins, and known gaps. |
| `policy.cfg` | Default policy that keeps built-ins permissive (can be tightened per deployment). |
| `syntax.json` | Legacy lexer keyword set (used by tooling/tests). |
| `docs/` | Human-facing guides (`guide.md` plus the docs you are reading). |
| `examples/` | Source of truth `.0bs` samples (HTTP, window demos, unit arithmetic, debugging, etc.). |
| `include/` | Public headers that expose the language pipeline, runtime, policy, and versioning. |
| `src/` | Implementation of the compiler, runtime, built-ins, and support tooling. |
| `tools/` | Developer tooling (currently the PowerShell script that rebuilds the VSIX). |
| `vscode/` | The VS Code extension (client + language server + grammar) and semantic helpers. |
| `erelang-language/` | Lightweight VS Code language configuration that ships to the marketplace. |
| `ui/winui3/`, `win/` | Windows-specific manifests and prototype UI scaffolding. |
| `build/` | Generated binaries, static libraries, and intermediate CMake artefacts (out of source). |

### Public headers (`include/erelang`)

| Header | Summary |
| --- | --- |
| `ast.hpp` | Placeholder AST wrapper types and future bytecode hook. |
| `cabi.h` | C ABI exported interface for embedding (`ob_run_file`, etc.). |
| `error.hpp` | Simple diagnostic struct plus reporter helpers. |
| `ffi.hpp` | FFI call shim returning `FFIResult`; currently returns `false` pending implementation. |
| `gc.hpp` | GC stub (ready for future mark/sweep implementation). |
| `lexer.hpp` | Token definitions, `LexerOptions`, and the polymorphic-identifier aware lexer API. |
| `modules.hpp` | Embedded/dynamic module registry API and `resolve_imports` helper. |
| `optimizer.hpp` | Constant folder interface returning fold statistics. |
| `parser.hpp` | Full AST node set, parser class, and statement/expression definitions. |
| `policy.hpp` | `Policy` struct plus singleton `PolicyManager` with `load`/`is_allowed`. |
| `runtime.hpp` | `Runtime` class contract: `run`, `run_single_action`, GUI hook, builtin evaluation. |
| `runtime_internals.hpp` | Shared containers for list/dict handles used by built-ins. |
| `stdlib.hpp` | Stub for retrieving packaged standard-library snippets. |
| `symboltable.hpp` | Minimal symbol table with add/find (parser/typechecker share this). |
| `typechecker.hpp` | Type checker data structures, diagnostics, and builtin metadata registry. |
| `version.hpp` | Central version constants and feature toggles (e.g., `ERELANG_FEATURE_THREADS`). |

### Core sources (`src/`)

| File | Purpose |
| --- | --- |
| `ast.cpp` | Stub implementation of `ast_wrap_program`. |
| `build_launcher.cpp` | Windows GUI front-end for invoking CMake builds (dark-themed control panel). |
| `build_launcher` GUI includes log streaming, targets toggles, and DPI-aware repaint logic. |
| `cabi_exports.cpp` | Implements the exported C ABI: loads programs, resolves imports, typechecks, optimises, then runs via `Runtime`. |
| `error.cpp` | Placeholder for richer diagnostic formatting. |
| `ffi.cpp` | FFI shim returning `{ value:"", ok:false }` until the bridge is filled in. |
| `gc.cpp` | Stub GC entry point. |
| `lexer.cpp` | Concrete lexer implementation: handles directives (`#include`), durations, unit numbers, polymorphic identifiers, doc comments, and comment nesting. |
| `main.cpp` | `obc` compiler entry: batches imports, merges modules, runs `Runtime::run` (static build). |
| `modules.cpp` | Embedded/dynamic module registration and Windows `.odll` loader. |
| `erelang_main.cpp` | CLI driver (`erelang.exe`): run, debug mode, compile to exe, packaging of bootstrap sources. |
| `optimizer.cpp` | Constant-folding visitor that repeatedly simplifies arithmetic expressions. |
| `parser.cpp` | Recursive-descent parser covering all language constructs (actions, hooks, entities, control flow, polymorphic identifiers). |
| `policy.cpp` | Implementation of `PolicyManager::load` with INI-style parsing and quotas. |
| `runtime.cpp` | Heart of the interpreter: execution engine, builtin dispatch, GUI integration, Windows control toolkit, deterministic tooling, list/dict storage. |
| `runtime.cpp` also wires builtins (`math`, `network`, `crypto`, `data`, `regex`, `perm`, `binary`, `threads`, `monitor`) and respects the policy gate. |
| `stdlib.cpp` | Stub returning empty module strings (placeholder for packaged stdlib). |
| `symboltable.cpp` | Placeholder for richer symbol table implementation. |
| `typechecker.cpp` | Semantic analysis: scopes, diagnostics (`TC***` codes), builtin registry (mirrors `builtins/` functions), unused warnings. |

### Built-in modules (`src/builtins`)

| Builtin file | What it exposes |
| --- | --- |
| `binary.cpp` | Binary buffer handles (`bin_new`, `bin_from_hex`, push/get u8, hex serialization). |
| `crypto.cpp` | FNV-1a hash, deterministic random byte generator (with simple xorshift). |
| `data.cpp` | In-memory key/value stores with disk persistence (`data_save`, `data_load`). |
| `math.cpp` | Integer arithmetic, trig, Collatz sweep reports, and numeric helpers. |
| `monitor.cpp` | File monitor subsystem with background threads, policy gating, interval control. |
| `network.cpp` | WinHTTP helpers: `http_get`, `http_download`, HLS playlist downloader, DNS/IP maintenance (`network.ip.*`), URL encoding. |
| `perm.cpp` | In-memory permission registry (`perm_grant`, `perm_revoke`, `perm_has`, `perm_list`). |
| `regex.cpp` | Regex match/find/replace wrappers around `std::regex`. |
| `threads.cpp` | High-level thread manager (spawn, join, timeouts, state query, GC, waits) with policy integration. |

### Other noteworthy code

| Area | Notes |
| --- | --- |
| `src/info/planned.obmanifest` | Metadata stub for planned manifest packaging. |
| `src/build_launcher.cpp` + resources | Provide a native build orchestrator with themed UI and command capture. |
| `win/erelang.manifest`, `win/erelang.rc` | Windows application manifest and resource script for `erelang.exe`. |
| `ui/winui3/` | Future WinUI 3 experiments (currently placeholders). |
| `build/erelang_compile` | Generated CMake project used by `erelang` during debug builds. |

## Language and pipeline features

### Lexing

* `lexer.cpp` honours options from `LexerOptions`: durations (`250ms`, `2m30s`), arbitrary unit numbers (`9.81m/s^2`), polymorphic identifiers (`name#tag` – tag stored in `Token::tag`), doc comments, nested block comments, and a `#include` pre-processor that is rewritten into `import` tokens.
* Tokens cover an extended operator set: `??`, `=>`, `**`, shifts, compound assignments, doc comments, and keywords loaded from `LexerOptions::keywords` (defaults include `action`, `entity`, `hook`, `parallel`, etc.).

### Parsing

* `parser.cpp` recognises the full AST described in `parser.hpp`: actions with parameters and return types, entities with fields/methods, hooks, globals, imports, and `run` directives.
* Statement coverage (from the parser and manifest) includes `let/const`, assignment, `print`, `sleep`, `return`, `if/else`, `switch`, `while`, classical and `for ... in ...` loops, `parallel`, `wait all` (parsed, runtime TODO), `pause`, `input`, method calls, member assignment, `fire`, and action/builtin invocations.
* Expressions support string, numeric, duration, unit, boolean literals; `new Type(...)`, member access, unary/binary operations, and function calls. Null-coalesce (`??`) is parsed into `BinOp::Coalesce`.
* Attributes (e.g., `@strict`, `@entry("main")`, `@event("onClick")`) are attached to actions/entities/hooks/globals as `Attribute` vectors for downstream consumers.

### Typechecking

* `typechecker.cpp` builds scopes via `ScopeManager`, tracks variable usage, enforces redeclaration rules (TC030), and ensures return paths (`ReturnFlow` analysis).
* `TypeChecker::init_builtins` mirrors most runtime built-ins, attaching min/max arity and return types; warnings highlight when runtime implements more than the checker currently knows (documented in `implemented.manifest`).
* Diagnostics surface as `TC***` codes (e.g., `TC001` unknown action, `TC070` unreachable code, `TC121` missing return).
* Entity field/method duplication checks run under `pass_check_program`, and `@strict` visibility is enforced in the runtime when invoking internal actions.

### Optimisation

* `optimizer.cpp` performs constant folding on arithmetic expressions across statements, running to a fixed point per block and tracking the number of folds/passes.

### Runtime

* `runtime.cpp` drives execution: it keeps a `globalVars_` map, executes actions (`ExecContext`), wires built-in dispatch via helper functions in `src/builtins/`, respects deterministic replay flags (`--deterministic`, `--seed`), and hosts the Windows GUI subsystem (window creation, DPI scaling, event routing).
* Lists and dictionaries are represented via global handle maps (`g_lists`, `g_dicts`), exposed to built-ins and iterators.
* GUI helpers expose `win_*` built-ins (create buttons, sliders, text boxes, message boxes, `win_loop`) and integrate with thread-safe policy gating.
* Policy checks wrap every builtin invocation, using `PolicyManager::instance().is_allowed`.

### Concurrency and monitoring

* `threads.cpp` offers a rich management API (handles, join with timeout, GC, state queries, `thread_wait_all`, policy-gated detach).
* `monitor.cpp` adds file monitoring, hashing, and interval control; results are stored in shared lists for script queries.

### Modules and imports

* `modules.cpp` provides `register_embedded_module`, `get_registered_modules`, and Windows dynamic module loading from neighbouring `.odll` files.
* Every loader (`erelang_main.cpp`, `cabi_exports.cpp`, `Runtime` bootstrap) normalises import paths, resolves local/embedded modules, and materialises module files into the temp directory when needed.

### Policy and security

* `policy.cpp` and `policy.hpp` parse `policy.cfg` once per process, supporting allow/deny lists and quotas (`max_threads`, `max_list_items`).
* Builtin entry points (`threads`, `monitor`, `runtime.cpp` dispatch) consult the policy before doing any work.

## Tooling and executables

| Binary | Source | Highlights |
| --- | --- | --- |
| `erelang.exe` | `src/erelang_main.cpp` | Unified CLI runner: run scripts, enable debug driver, compile to standalone exe (`--compile`). Uses static compiler pipeline when `ERELANG_STATIC_RUNNER` is defined, otherwise loads `liberelang`. |
| `obc.exe` | `src/main.cpp` | Simpler compiler driver for batch runs; resolves `#include` directives to `import`s and merges dependencies. |
| `build.exe` | `src/build_launcher.cpp` | Dark-themed Windows builder for developers (build/rebuild/clean, target toggles). |

## Built-in catalogue

The runtime exposes a broad set of built-ins (documented in `typechecker.cpp` and `implemented.manifest`). The highlights by category:

* **Core / environment**: timestamps (`now_ms`, `now_iso`), environment variables, machine identity (`username`, `machine_guid`, `hwid`), CLI args, process execution (`exec`, `run_file`, `run_bat`).
* **Filesystem**: text IO, directory creation, copy/move/delete, path queries, listing directories into list handles.
* **Collections**: list and dict factories with push/get/len/keys/values, plus helpers for joining and clearing.
* **Math**: arithmetic, trig, power, abs/min/max, Collatz sweep reports.
* **Crypto**: FNV-1a hashing, deterministic random byte hex generation.
* **Networking**: WinHTTP-based GET/download, HLS best-variant download, DNS/IP maintenance helpers, URL encoding.
* **Binary buffers**: create, append/get bytes, convert to/from hex.
* **Permissions**: simple in-memory grant/revoke/list (meant for coarse capability gating inside scripts).
* **Threads**: spawn (with optional detach), join, join with timeout, GC, wait all, state query, removal, yield.
* **Monitoring**: watch file paths and query last change metadata.
* **GUI (Win32)**: `win_*` primitives for windows/controls.
* **Data store**: in-memory map handles with save/load to disk.

All handles are expressed as tagged strings (`list:<id>`, `dict:<id>`, `thread:<id>`, `monitor:<id>`, `data:<id>`, `bin:<id>`), and the runtime keeps lookup tables in `runtime_internals.hpp` globals.

## Examples and documentation

* `docs/guide.md` – Introductory language manual (actions, variables, control flow, collections, GUI).
* `examples/*.0bs` – Working samples: networking (`download_file.0bs`, `download_best_hls.0bs`), network diagnostics, window demos, duration math, unit testing, threading, data store usage.
* `examples/std/` – Standard library-style helpers imported by scripts.
* `implemented.manifest` – Up-to-date feature checklist (parsed statements, built-ins, missing runtime behaviours such as `wait all`).

## VS Code tooling

* `vscode/erelang-vscode` – Full extension source:
  * `syntaxes/erelang.tmLanguage.json` – TextMate grammar aligned with lexer tokens (durations, units, polymorphic IDs).
  * `language-configuration.json` – Comment/bracket rules.
  * `server/src/server.ts` – Language server providing keyword/builtin completions, include suggestions, std module surfacing.
  * `client/src/extension.ts` – Registers Run / Run with Args / Compile commands, code lenses, and delegates to the server.
  * `package.json` – Commands, activation events, contributes sections for keybindings and grammars.
* `tools/build-and-update-extension.ps1` – Rebuilds TypeScript, packages the VSIX, and installs it via the `code` CLI.

## Known gaps (from `implemented.manifest`)

* `wait all`, `pause`, and `input` statements parse but lack runtime implementations.
* Logical `&&` token is absent; `||` exists.
* Null-coalescing (`??`) is parsed but currently unused downstream.
* Type checker does not yet know about every filesystem/network builtin – they still work at runtime but emit `TC021` (parameter mismatch) until synced.
* `language_version()` builtin returns empty until `version.hpp` wires it.
* `stdlib_get` and GC/FFI stubs need real implementations.
* Member access typing (`entity.field`) still resolves to `unknown` (no field type inference yet).

Keep `implemented.manifest` in sync whenever you land new features. The manifest, this overview, and the tutorial below are the three documents we expect to stay updated with each iteration.
