# erevos.fullguide.md

## Scope
This guide is a full technical map of the code currently in this repository, centered on all code in `src/` and the directly-related "and stuff" around it (`include/`, `CMakeLists.txt`, runtime/plugin wiring).

It covers:
- Build targets and how binaries are composed.
- Lexer/parser/typechecker/optimizer behavior.
- Runtime execution model and statement semantics.
- Builtins (core and external modules).
- Plugin loading, plugin manifest parsing, creation kit tooling.
- C ABI surface and module system.
- File-by-file notes for every file in `src`.
- Known placeholders, mismatches, and implementation gaps visible in code.

Date baseline: 2026-03-03.

## High-Level Architecture
The project builds around a C++20 scripting runtime and toolchain for Erelang.

Main stages:
1. Lex source into `Token`s (`lexer.cpp`).
2. Parse tokens into `Program` AST (`parser.cpp`).
3. Type-check with diagnostics (`typechecker.cpp`).
4. Optional constant-fold optimization (`optimizer.cpp`).
5. Execute with interpreter runtime (`runtime.cpp`).

Runtime supports:
- Actions, hooks, entities, globals.
- Dynamic list/dict/table handles.
- Windows-native UI controls (`win_*` and `ui_*`).
- Plugin metadata injection and hook dispatch.
- Builtin dispatch split between runtime TU and builtin component files.

## Build and Outputs (`CMakeLists.txt`)
Primary targets:
- `erelang` (static library): all compiler/runtime components.
- `obc` (CLI compiler/runner style frontend from `src/main.cpp`).
- `erelang_runner` (output name `erelang`, from `src/obs_main.cpp`).
- `build` (Windows GUI build launcher from `src/build_launcher.cpp`).

Optional runtime DLL modes:
- `BUILD_SHARED_RUNTIME=ON`: builds `erelang_shared` (`erelang.dll`).
- `SPLIT_RUNTIME_DLLS=ON`: legacy split-component DLL build.

Notable build behaviors:
- Release favors size (`-Os`, section GC, strip).
- Post-build creates aliases: `obs.exe` from runner, `libobskript.a` alias for static lib on Windows.
- Post-build tries creating `debug.exe` by running `erelang --make-debug`.
- Optional embed payload mode (`ERELANG_EMBED_PAYLOAD`) extracts static lib + headers into runner resources.
- VS Code extension packaging is invoked best-effort if extension dir exists.

## Public API and Data Model (`include/erelang/*`)
Core AST (`parser.hpp`):
- Expressions: string, number, bool, ident, binary/unary, `new`, member access, function call.
- Statements: print/sleep/call/parallel/wait/pause/input/fire/let/return/set/method call/if/switch/while/for/for-in.
- Decls: action/hook/entity/global/import.
- Visibility: `public`/`private`.
- Program directives: `strict`, `debug`, `runTarget`, plugin aliases.

Runtime (`runtime.hpp`):
- `Runtime::run`, `run_single_action`, `eval_builtin_call`, GUI click callback.
- Plugin records include metadata, core properties, hook bindings, optional DSL spec.
- Runtime stores global vars, current program pointer, interpolation expression cache.

Lexer API (`lexer.hpp`):
- Rich `TokenKind` set: operators, durations, unit numbers, doc comments, strict/bitwise-ish tokens.
- `LexerOptions` toggles comments/durations/units/poly identifiers/custom keyword set.

Type checker API (`typechecker.hpp`):
- Multi-pass checker with diagnostics (`TCxxx` codes).
- Scope manager and expression/statement visitors.

Other headers:
- `modules.hpp`: embedded/dynamic module registration and loading (`.odll`).
- `plugins.hpp`: plugin manifest discovery result model.
- `cabi.h`: C ABI (`ob_run_file`, `ob_collect_files`, `ob_free_string`).
- `version.hpp`: version 1.3.0 + feature macros.
- `policy.hpp`: allow/deny and quotas (default permissive).
- `runtime_internals.hpp`: exposes shared list/dict handle maps for builtin modules.

Legacy compatibility:
- `include/obskript/*` thin wrappers include `erelang/*` + alias namespace (`obskript = erelang`).

## Language Frontend

### Lexer (`src/lexer.cpp`)
Implements:
- Normal tokenization for punctuation/operators/keywords/literals.
- `#include` preprocessing at line start; converts include lines into `import` + optional `as` alias tokens.
- Nested block comments `/* ... */`, doc comments `///` and `/** */` when enabled.
- Raw strings `r"..."`, escaped normal strings/chars.
- Numeric lexing with hex/bin/oct/decimal/exponent forms.
- Duration tokens (e.g. `2m30s`) when enabled.
- Unit-number tokens (e.g. `9.81m/s^2`) when enabled.
- Poly identifiers with `#tag` split into token text + `tag` field.

Observations:
- Contains an unused function `canEXPR()` with no return path.
- Handles many operator token kinds that parser currently does not fully consume.

### Parser (`src/parser.cpp`)
Implements recursive descent:
- Expression precedence: coalesce -> or -> and -> equality -> relational -> additive -> multiplicative -> unary -> primary.
- Supports:
  - entity construction (`new Type(...)`)
  - member access and dotted function call paths
  - for, while, if/else-if, switch/case/default
  - parallel blocks and `wait all`
  - `pause`, `input`, `fire`
  - declarations: action/entity/hook/global/import/run
  - directives and attributes (`@...`)

Key semantics:
- Duration literal parser converts to milliseconds.
- `run <action>` sets `Program::runTarget`.
- Entity members can be fields or action methods with modifiers/attributes.
- Plugin glob import `/plugins/*/project.elp` marks `pluginGlob` and adds plugin alias.

Strictness behavior:
- Action parsing enforces explicit visibility (`public` or `private`) and throws if missing.

### Type Checker (`src/typechecker.cpp`)
Passes:
1. Collect symbols + builtin signature map.
2. Check duplicates, run target existence, statement and expression semantics.
3. Emit unused warnings for actions/entities/methods/variables.

Checks include:
- Use-before-declare.
- Redeclaration and const assignment checks.
- Return type compatibility and missing returns.
- Bool condition requirements for `if/while/for`.
- Unknown action/builtin call diagnostics and parameter count checks.

Builtin signatures are hardcoded and broad, including GUI/data/network/crypto/thread/monitor/plugin-core helpers.

### Optimizer (`src/optimizer.cpp`)
- Constant folds integer binary/unary expressions.
- Walks all action/method blocks recursively.
- Runs fixpoint passes until no more folds.
- Tracks `folds`, `nodesReplaced`, `passes`.

## Runtime Execution Engine (`src/runtime.cpp`)

### Core runtime state
- Global string map `globalVars_` and set `globalNames_`.
- Dynamic list store: `list:<id>` -> vector string.
- Dynamic dict store: `dict:<id>` -> map string->string.
- Current program pointer for callbacks/thread builtins.
- Plugin records and alias-injected objects/vars.

### Evaluation model
All runtime values are string-backed at execution time.

`eval_string` handles:
- interpolation in string literals with `{...}`:
  - local env vars
  - globals
  - parsed interpolation expression cache (mini parse via synthetic action)
- primitive expression evaluation
- binary arithmetic/comparison/logical/coalesce
- unit add/sub for matching suffixed unit strings
- member access and builtin function call expressions

### Statement execution (`exec_stmt`)
Supports all AST statements:
- `print`, `sleep`, parallel spawning and `wait all`, `pause`, `input`.
- `let` with entity construction (`new`) and optional `init` method call.
- `return` stores return flag + value in `ExecContext`.
- hook firing (`fire`), control flow (`if/while/for/for-in/switch`).
- assignment to vars/member fields with strict checks for builtin alias assignment.
- method calls on:
  - builtin module aliases
  - list/dict handle objects (dynamic methods)
  - native Window-backed methods
  - scripted entity methods with visibility + `@hidden` enforcement
- direct action call dispatch with fallback to builtin call by name.

### Program lifecycle (`run`)
Execution order:
1. Resolve entry action (`runTarget` or `main`).
2. Seed globals and directives into root env.
3. Seed plugin alias objects and builtin module aliases.
4. Dispatch plugin hooks in order: `datahook`, `onload`, `onstart`.
5. Run script hook `onStart` if present.
6. Run entry action.
7. Run `onEnd` (or alias `onExit`).
8. Dispatch plugin teardown hooks reverse order: `onend`, `onexit`, `onunload`.

### Single-action execution (`run_single_action`)
- Runs one action without full lifecycle.
- Lazily initializes globals + startup hooks once if needed.
- Persists global mutations.

### Plugin alias seeding
For each plugin alias from imports:
- Creates pseudo object `alias:slug` with fields (`id`, `name`, `version`, hook metadata, core values, language DSL metadata).
- Adds `alias.slugs`, `alias.count`, `alias.core.files`, and per-file core keys vars.

### Windows GUI bridge
- Registers Win32 window class once per process instance.
- Creates native windows and controls.
- Supports custom layout engine and parsed `erelang.ui` document loader (`ui_load`).
- Routes `WM_COMMAND`/`WM_HSCROLL` to runtime action callbacks via `handle_gui_click`.

## Builtins

### Builtins inside `runtime.cpp`
Core/system:
- `now_ms`, `now_iso`, `env`, `username`, `computer_name`, `machine_guid`, `volume_serial`, `hwid`, `uuid`, `rand_int`.
- deterministic helpers via CLI seed flags: `advance_time`, deterministic `now_ms`.
- metadata/helpers: `dev_meta`, `audit`, language info builtins.

CLI helpers:
- `args_count`, `args_get`.
- `exec`, `run_file`, `run_bat`, `read_line`, `prompt`.

Filesystem/path:
- `read_text`, `write_text`, `append_text`, `file_exists`, `mkdirs`, `copy_file`, `move_file`, `delete_file`, `list_files`, `cwd`, `chdir`, `path_join`, `path_dirname`, `path_basename`, `path_ext`.

Containers:
- list: `list_new`, `list_push`, `list_get`, `list_len`, `list_join`, `list_clear`, `list_remove_at`.
- dict: `dict_new`, `dict_set`, `dict_get`, `dict_has`, `dict_keys`, `dict_values`, `dict_get_or`, `dict_remove`, `dict_clear`, `dict_size`, `dict_merge`, `dict_clone`, `dict_items`, `dict_entries`, path variants.
- table-over-dict: `table_new`, `table_put`, `table_get`, `table_has`, `table_remove`, `table_rows`, `table_columns`, `table_row_keys`, `table_clear_row`, `table_count_row`.

Plugin-core queries:
- `plugin_core`, `plugin_core_files`, `plugin_core_keys`.

UI/Win32:
- `win_*` and `ui_*` sets (window/control create/read/write/events/layout/message loop, UI file loader).

### External builtin modules (`src/builtins/*.cpp`)
- `math.cpp`: integer math + trig + `collatz_*` sweep stats.
- `network.cpp`: `http_get`, `http_download`, `hls_download_best`, URL encode, Windows `ipconfig` actions, network debug log/state helpers.
- `system.cpp`: process execution wrappers (`system.cmd`, `system.execute`, output + last exit code, `system.ip.flush`).
- `crypto.cpp`: FNV-1a hash and xorshift-based random-bytes hex.
- `data.cpp`: in-memory key/value stores with simple file save/load.
- `regex.cpp`: match/find/replace via `std::regex`.
- `perm.cpp`: in-memory permission grant/revoke/query/list.
- `binary.cpp`: binary buffer handles and byte/hex ops.
- `threads.cpp`: thread lifecycle manager with states, join timeout, detach policy gating, GC/purge/remove/state builtins.
- `monitor.cpp`: file-monitor handles with polling, hash/mtime/size change tracking.
- `render.cpp`: currently empty translation unit.

Dispatch behavior:
- Runtime calls external dispatchers in sequence and returns first non-empty result.
- Void side-effect builtins often return empty string.

## Plugin System

### Discovery and manifest parsing (`src/plugins.cpp`)
- Scans built-in plugin root (`<exe>/plugins`) and user plugin root.
- User plugin root:
  - Windows: `%LOCALAPPDATA%\Erelang\Plugins` fallback `%APPDATA%...`
  - Unix-like: `XDG_DATA_HOME` or `~/.local/share/erelang/plugins`
- Requires `project.elp`.
- Parses nested XML-like blocks:
  - `<plugin>`, `<erelang_manifest>`, `<content>`, optional `<language>`, `<hooks>`, `<dependencies>`
- Collects script/core/asset includes; parses `.core` key/value files.
- Supports hook binding maps and DSL extension keyword alias maps.
- Duplicate plugin `id` is overridden by later-discovered source.

### Creation Kit (`src/creation_kit.cpp`)
Interactive CLI utility:
- Plugin template wizard (manifest + bootstrap script + README).
- Syntax profile editor from baseline `syntax.json` and output `syntax.override.json`.
- Runtime policy override editor (`policy.override.cfg`).

## Module System (`src/modules.cpp`)
- Maintains registry of embedded `ModuleDef` values.
- `resolve_imports` currently a simple pass-through placeholder.
- Windows dynamic load support scans `.odll` files and calls exported `ErelangGetModule`.
- Loaded DLL handles are retained for process lifetime.

## CLI and Executables

### `src/main.cpp` (`obc`)
- Loads plugins and registers DSL specs from plugins.
- Recursively loads imports and `#include` dependencies.
- Merges loaded modules into final main program.
- Static checks (warn if no main/run target).
- Runs via `Runtime`.
- Supports `--creation-kit` / `kit` command branch.

### `src/obs_main.cpp` (`erelang` runner)
Major modes:
- run script: `erelang <file> [--debug]`.
- compile standalone exe: `--compile ...` (static default, dynamic optional).
- make debugger exe: `--make-debug`.
- list builtins/help/about/bootstrap/version.

Compile flow (`--compile`):
- Collect reachable files (and optional `.olib`/`.ol` library manifests/files).
- Generate temporary bootstrap C++ project with embedded source map.
- Configure/build temp CMake app and copy `app.exe` to output.
- Copy `.odll` files and write `manifest.erelang` metadata.
- Best-effort strip and temp cleanup.

### `src/build_launcher.cpp`
- Windows GUI app for build/rebuild/clean with log output.
- Uses `cmake` commands via `_popen`/`system` flow.
- Dark-themed owner-draw UI controls and status indicator.
- Supports `--json` placeholder backend mode.

## C ABI Layer (`src/cabi_exports.cpp`)
Exports:
- `ob_run_file`: end-to-end load/import/typecheck/optimize/run for one entry file.
- `ob_collect_files`: callback over transitive dependency files.
- `ob_free_string`: frees ABI-allocated error string.

Import resolution includes:
- local relative/absolute files
- extension fallback `.0bs` / `.obsecret`
- registered module materialization to temp files

## EroDSL Helpers (`src/erodsl/spec.cpp`)
- Default spec id/name/version and default extensions.
- Canonical keyword set and alias application.
- Builds lexer options from spec.
- Normalizes extension strings.

## Policy System
- `include/erelang/policy.hpp`: header-implemented singleton with load/is_allowed and quotas.
- `src/policy.cpp`: additional implementation also present.

Practical effect:
- Builtin calls are policy-gated early in runtime.
- Monitor/thread modules also check policy explicitly by builtin name.

## File-by-File Coverage (`src/`)
- `src/ast.cpp`: AST wrapper stub (`ast_wrap_program`).
- `src/build_launcher.cpp`: Win32 build GUI app.
- `src/cabi_exports.cpp`: C ABI run/collect bridge.
- `src/creation_kit.cpp`: interactive plugin/syntax/policy generator/editor.
- `src/error.cpp`: placeholder for diagnostics formatting.
- `src/ffi.cpp`: stub FFI call returning `{ "", false }`.
- `src/gc.cpp`: placeholder GC TU.
- `src/language_profile.cpp`: compatibility TU for legacy symbols.
- `src/lexer.cpp`: tokenization/preprocessing implementation.
- `src/main.cpp`: `obc` frontend and loader.
- `src/modules.cpp`: module registry and dynamic `.odll` loading.
- `src/obs_main.cpp`: main runner CLI and compiler packager.
- `src/optimizer.cpp`: constant folding optimizer.
- `src/parser.cpp`: parser and AST construction.
- `src/plugins.cpp`: plugin discovery and manifest parser.
- `src/policy.cpp`: extra policy manager implementation.
- `src/runtime.cpp`: interpreter runtime, builtins, GUI bridge.
- `src/stdlib.cpp`: stdlib placeholder dispatch stub.
- `src/symboltable.cpp`: symbol table placeholder TU.
- `src/typechecker.cpp`: semantic checks and diagnostics.
- `src/erodsl/spec.cpp`: DSL spec + keyword alias helpers.
- `src/builtins/binary.cpp`: binary handle builtins.
- `src/builtins/crypto.cpp`: hash/random builtins.
- `src/builtins/data.cpp`: key/value store builtins.
- `src/builtins/math.cpp`: math + collatz builtins.
- `src/builtins/monitor.cpp`: monitor subsystem builtins.
- `src/builtins/network.cpp`: HTTP/HLS/network system builtins.
- `src/builtins/perm.cpp`: permission set builtins.
- `src/builtins/regex.cpp`: regex builtins.
- `src/builtins/render.cpp`: empty TU.
- `src/builtins/system.cpp`: process/system builtins.
- `src/builtins/threads.cpp`: managed thread builtins.
- `src/info/planned.obmanifest`: project metadata + roadmap.

## Known Gaps and Notable Mismatches
- Multiple source files are explicit placeholders/stubs (`ast`, `error`, `gc`, `ffi`, `stdlib`, `symboltable`, `render`).
- `lexer.cpp` has an unused incomplete helper `canEXPR()`.
- Policy manager logic exists both header-inline and in `src/policy.cpp` (duplication risk).
- Builtin inventories differ somewhat between typechecker signatures and runtime/dispatch implementations.
- Runtime values are string-based; no strict runtime typed value model yet.
- Most GUI paths are Windows-only (`#ifdef _WIN32`).

## Included "And Stuff" Coverage
Also reviewed and tied into this guide:
- `include/erelang/*.hpp` public API model.
- `include/obskript/*.hpp` compatibility wrappers.
- `CMakeLists.txt` target graph and build behavior.
- `README.md`/manifest context where it aligns with actual code paths.
