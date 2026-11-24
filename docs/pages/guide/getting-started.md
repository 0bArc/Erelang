# Getting started

## Prerequisites

- Windows 10+, Linux, or macOS 11+
- CMake 3.21+
- C++20 compiler: MinGW-w64 on Windows, GCC 11+ or Clang 14+ elsewhere
- Linux: libcurl and OpenSSL development packages (`sudo apt install libcurl4-openssl-dev libssl-dev`)
- macOS: Xcode command line tools and OpenSSL (`brew install openssl@3`)

## Build

Windows:

```bash
cmake --preset debug
cmake --build --preset debug --target erelang_runner -j 8
```

Linux and macOS:

```bash
cmake --preset unix-debug
cmake --build --preset unix-debug --target erelang_runner -j 8
```

Optional flags:

- `-DERELANG_EXPERIMENTAL=ON`: enable `builtin/threads` and `builtin/monitor`
- `release` (Windows) or `unix-release`: optimized build in `build/release`

Binary location: `build/debug/bin/Debug/erelang` (`erelang.exe` on Windows; release builds in `build/release/bin/Release/`)

## Run a script

```bash
./build/debug/bin/Debug/erelang.exe examples/program.elan
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

1. [language.md](language.md): syntax reference  
2. [filesystem.md](../modules/filesystem.md): read and write files  
3. [imports.md](imports.md): use `builtin/*` modules  
4. [core-builtins.md](../modules/core-builtins.md): built-in functions  

VS Code extension: `erevos-language/`: install the `.vsix` or build from source with `npm run package` inside that folder.
