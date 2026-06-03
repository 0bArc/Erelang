# Toolchain

## Binaries

| Binary | Source | Role |
|--------|--------|------|
| `erelang.exe` | `src/obs_main.cpp` | Run scripts, the main binary you use |
| `obc.exe` | `src/main.cpp` | Compiler driver (emit IR, emit ASM) |
| `erelang` static lib | CMake target `erelang` | Compiler + runtime, linkable |

## Pipeline

1. **Lexer** (`lexer.cpp`) — tokens, `#include <builtin/x>` → import node
2. **Parser** (`parser.cpp`) — AST `Program`
3. **Typechecker** (`typechecker.cpp`) — `TC***` diagnostics
4. **Optimizer** (`optimizer.cpp`) — constant folding
5. **Runtime** (`runtime*.cpp`, `builtins/*.cpp`) — interpretation

## CLI

```bash
# Run a script
erelang script.elan

# Emit intermediate representation
erelang --emit-ir script.elan --out script.eir

# Emit x64 assembly
erelang --emit-asm script.elan --out script.asm

# Help
erelang --help
```

## CMake options

| Flag | Effect |
|------|--------|
| `ERELANG_EXPERIMENTAL=ON` | threads + monitor builtins |
| `BUILD_SHARED_RUNTIME=ON` | build `erelang.dll` |
| `ERELANG_EMBED_PAYLOAD=ON` | embed static lib in runner for portability |
| `ERELANG_BUNDLE_MINGW_RUNTIME=ON` | copy MinGW DLLs next to exe |

## Plugins

Plugins live next to the executable in `plugins/` or in `%LOCALAPPDATA%\Erelang\Plugins` (Windows).

Each plugin folder contains a `project.elp` manifest. The runtime auto-loads plugins and calls `onLoad` hooks.

## VS Code extension

```powershell
.\tools\install-erelang-extension.ps1
```

Or build from source inside `erevos-language/`:

```bash
npm install
npm run package
```

This produces `erelang_language.vsix` — install it in VS Code via **Extensions → Install from VSIX**.

## Source map (contributors)

| Area | Path |
|------|------|
| AST types | `include/erelang/parser.hpp` |
| Typechecker | `src/typechecker.cpp` |
| Builtin dispatch | `src/runtime_builtins.cpp` |
| Import aliases | `src/runtime_imports.cpp` |
| Module impls | `src/builtins/*.cpp` |
| C ABI | `include/erelang/cabi.h`, `src/cabi_exports.cpp` |

## Related

- [getting-started.md](getting-started.md)
- [imports.md](imports.md)
- [diagnostics.md](diagnostics.md)
