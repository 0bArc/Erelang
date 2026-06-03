# Getting started

## Prerequisites

- Windows 10+ (primary target today)
- CMake 3.20+
- C++20 toolchain (MSVC, MinGW, or Clang)

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target erelang_runner -j 8
```

Optional flags:

- `-DERELANG_EXPERIMENTAL=ON` — enable `builtin/threads` and `builtin/monitor`
- `-DCMAKE_BUILD_TYPE=Release` — smaller, optimized binary

Binary location: `build/bin/Debug/erelang.exe` (or `Release/` for release builds)

## Run a script

```bash
./build/bin/Debug/erelang.exe examples/program.elan
```

Every runnable file needs a `run` target:

```elan
@erelang

public action main {
    print "Hello, world!";
}

run main;
```

## Emit IR or assembly

```bash
erelang --emit-ir examples/program.elan --out program.eir
erelang --emit-asm examples/program.elan --out program.asm
```

## File layout

| Extension | Role |
|-----------|------|
| `.elan` | Erelang source file |
| `project.elp` | Plugin manifest (optional) |

The `@erelang` directive at the top of a file is recommended but not required.

## Next steps

1. [language.md](language.md) — syntax reference  
2. [filesystem.md](filesystem.md) — read and write files  
3. [imports.md](imports.md) — use `builtin/*` modules  
4. [core-builtins.md](core-builtins.md) — built-in functions  

VS Code extension: `erevos-language/` — install the `.vsix` or build from source with `npm run package` inside that folder.
