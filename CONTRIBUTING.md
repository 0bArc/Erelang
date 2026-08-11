# Contributing to Erelang

## Getting started

1. Fork the repository and clone your fork.
2. Build from source following the instructions in [README.md](README.md).
3. Run the test examples to confirm your setup works:

```bash
./build/bin/Debug/erelang.exe examples/language_kitchen_sink.elan
./build/bin/Debug/erelang.exe examples/feature_pass_v2.elan
```

## Code style

- C++20. Use `std::string_view` for string parameters that are not stored.
- Prefer `auto` when the type is obvious from the right-hand side.
- Use `const` everywhere the value does not change.
- Indentation: 4 spaces. No tabs.
- Opening brace on the same line as the statement.

```cpp
if (condition) {
    do_something();
}
```

- Include order: standard library headers first, then project headers.
- Use forward declarations in headers where possible to reduce compile time.

## Project structure

```
src/
  lexer.cpp           Lexing and tokenization
  parser.cpp          AST construction
  typechecker.cpp     Static type analysis
  optimizer.cpp       AST optimization passes
  ir.cpp              Intermediate representation

  runtime/
    core.cpp          Program entry and lifecycle
    helpers.cpp       Shared container state (lists, dicts, sets, queues)
    eval.cpp          Expression evaluation
    actions.cpp       Statement execution and handle dispatch
    builtins.cpp      Core builtins and import-gated module dispatch
    imports.cpp       Module alias binding (compile-time path-to-builtin mapping)
    plugins.cpp       Plugin loading and manifest discovery

    builtins/          Per-module builtin implementations
      math.cpp
      network.cpp
      system.cpp
      regex.cpp
      crypto.cpp
      data.cpp
      binary.cpp
      websocket.cpp
      performance.cpp

    features/
      serialization.cpp

  obs_main.cpp        CLI entry point (runner, compiler, bootstrap)
```

## Builtin modules

Builtins are organized into three layers:

1. **Fundamental operations** (no import, always available): `print`, `+`, `-`, `*`, `/`, `%`, `if`, `while`, `for`.

2. **Core types** (no import, always available): `int()`, `float()`, `string()`, `bool()`, `time.now_ms()`, `time.uuid()`, `env.get()`, `io.read_line()`, `json.encode()`, `json.decode()`, `lang.name()`, `lang.version()`, `plugin.core()`, plus handle types (`list.*`, `dict.*`, `set.*`, `queue.*`, `file.*`, `ptr.*`, `strbuf.*`).

3. **Import-gated modules** (require `#include <builtin/...> as alias`): `builtin/fs`, `builtin/network`, `builtin/regex`, `builtin/crypto`, `builtin/data`, `builtin/binary`, `builtin/process`, `builtin/websocket`, `builtin/math`, `builtin/debug`, `builtin/performance`, `builtin/threads` (experimental), `builtin/monitor` (experimental).

### Adding a new builtin module

1. Create `src/runtime/builtins/new_module.cpp` with a dispatch function:

```cpp
#include <string>
#include <vector>

namespace erelang {

static std::string dispatch(const std::string& name,
                            const std::vector<std::string>& argv) {
    if (name == "mymod.do_thing") {
        // implementation
        return {};
    }
    return {};
}

std::string __erelang_builtin_mymod_dispatch(
    const std::string& name,
    const std::vector<std::string>& argv) {
    return dispatch(name, argv);
}

} // namespace erelang
```

2. Declare the dispatch function in `src/runtime/builtins.cpp`:

```cpp
std::string __erelang_builtin_mymod_dispatch(
    const std::string& name,
    const std::vector<std::string>& argv);
```

3. Wire it into `dispatch_imported_builtin_modules` in `src/runtime/builtins.cpp`:

```cpp
if (program_imports_module(program, "builtin/mymod")) {
    if (auto r = __erelang_builtin_mymod_dispatch(name, argv); !r.empty()) {
        return r;
    }
}
```

4. Add `resolve_builtin_module_method` and `bind_builtin_module_aliases` entries in `src/runtime/imports.cpp` so `#include <builtin/mymod> as mm` maps `mm.method` to the dispatch name.

5. Add the source file to `CMakeLists.txt` in both the static and shared library sections.

## Type checking

The typechecker runs at compile time on every program. Key principles:

- Type mismatches are **hard errors**, not warnings.
- `any` is **opt-in only**. The typechecker must never infer `any` as a fallback.
- When a type cannot be determined, the typechecker emits a diagnostic and the compile fails.

When adding a new builtin, update the `TypeChecker` in `src/typechecker.cpp` to register the function signature in the builtin type registry at the end of the file (search for the `add(...)` calls).

## Testing

Before submitting a PR, confirm:

1. The full build passes:

```bash
cmake --build build --config Release
cmake --build build --config Debug
```

2. Existing examples still run:

```bash
./build/bin/Debug/erelang.exe examples/language_kitchen_sink.elan
./build/bin/Debug/erelang.exe examples/feature_pass_v2.elan
./build/bin/Debug/erelang.exe examples/program.elan
```

3. If adding a new feature, include an example file under `examples/` that exercises it.

## Commit messages

- Use the present tense ("Add X" not "Added X").
- Start with a capital letter, no trailing period.
- Keep the subject line under 72 characters.
- For larger changes, include a body that explains what and why.

Example:

```
Add debug.timer_start and debug.timer_stop builtins

Named timers use std::chrono::steady_clock and store start
times in a static map. Timer output goes to stderr.
```

## Pull requests

- Target the `main` branch.
- Keep PRs focused. One feature or fix per PR.
- Update documentation if your change adds, removes, or renames a builtin.
- Mention any new diagnostic codes introduced.

## Need help?

Open an issue for questions about the architecture, build system, or language design. The [docs/](docs/) directory contains topic guides for each area of the language.
