# Toolchain

## Binaries

| Binary | Source | Role |
|--------|--------|------|
| `erelang.exe` | `src/obs_main.cpp` | Run scripts, the main binary you use |
| `obc.exe` | `src/main.cpp` | Compiler driver (emit IR, emit ASM) |
| `erelang` static lib | CMake target `erelang` | Compiler + runtime, linkable |
| `symcheck.exe` | `tools/symcheck` ([0bArc/symcheck](https://github.com/0bArc/symcheck)) | PE/COFF symbol and linker diagnostics |

## Pipeline

1. **Lexer** (`lexer.cpp`): tokens, `#include <builtin/x>` -> import node
2. **Parser** (`parser.cpp`): AST `Program`
3. **Typechecker** (`typechecker.cpp`): `TC***` diagnostics
4. **Optimizer** (`optimizer.cpp`): constant folding
5. **Runtime** (`runtime*.cpp`, `builtins/*.cpp`): interpretation

## CLI

```bash
# Run a script
erelang script.elan

# Format a script (subset: indent, braces, action layout)
erelang --fmt script.elan
erelang --fmt --check script.elan   # exit 1 if would change

# Emit markdown API list from public actions/structs/enums
erelang --docgen script.elan
erelang --docgen script.elan --out api.md

# Package lock / fetch (local registry)
erelang --lock package.elan --registry ./registry
erelang --fetch package.elan --registry ./registry

# Emit intermediate representation
erelang --emit-ir script.elan --out script.eir

# Emit x64 assembly (Windows x64 calling convention; --build-native links it on Windows only)
erelang --emit-asm script.elan --out script.asm

# Help
erelang --help
```

The VS Code extension (`erevos-language/`) runs a separate language-server process and includes a
DAP debugger. Compiler diagnostics analyze unsaved buffers through `--check-stdin <file>` without
writing a temporary file; imports resolve relative to that file's location. Compiler token positions
feed a shared lexical binding index used for local editor analysis. This is not yet an export of
the typechecker's full inferred-type graph. Navigation can follow direct script imports; project
references and action renames discover closed workspace files on explicit requests, bounded to 2,000
files / 8 MB. Larger projects are rejected rather than partially renamed. No workspace scan runs on edits.

## CMake options

| Flag | Effect |
|------|--------|
| `ERELANG_EXPERIMENTAL=ON` (default) | threads + monitor builtins |
| `ERELANG_EXPERIMENTAL=OFF` | stubs return `error:experimental_disabled` |
| `BUILD_SHARED_RUNTIME=ON` | build `erelang.dll` |
| `ERELANG_EMBED_PAYLOAD=ON` | embed static lib in runner for portability |
| `ERELANG_BUNDLE_MINGW_RUNTIME=ON` | copy MinGW DLLs next to exe |
| `ERELANG_BUILD_SYMCHECK=OFF` | skip the SymCheck CLI under `tools/symcheck` |

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

This produces `erelang_language.vsix`: install it in VS Code via **Extensions -> Install from VSIX**.

## Source map (contributors)

| Area | Path |
|------|------|
| AST types | `include/erelang/parser.hpp` |
| Typechecker | `src/typechecker.cpp` |
| Builtin dispatch | `src/runtime/builtins.cpp` |
| Import aliases | `src/runtime/imports.cpp` |
| Module impls | `src/runtime/builtins/*.cpp` |
| C ABI | `include/erelang/cabi.h`, `src/runtime/cabi_exports.cpp` |

## Related

- [getting-started.md](getting-started.md)
- [imports.md](imports.md)
- [packages.md](packages.md)
- [diagnostics.md](diagnostics.md)
