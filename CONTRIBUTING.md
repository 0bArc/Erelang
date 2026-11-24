# Contributing

Build with CMake, then run a few examples:

```bash
cmake --preset debug          # Linux and macOS: cmake --preset unix-debug
cmake --build --preset debug --target erelang_runner -j 8
./build/debug/bin/Debug/erelang.exe examples/program.elan
pwsh tests/run.ps1
```

Code that touches the operating system (processes, sockets, HTTP, TLS, crypto, paths) must build and behave the same on Windows, Linux and macOS. CI runs the tests on Linux and macOS.

C++20, 4-space indent, braces on the same line. Prefer the standard library. Do not add comments that restate the code.

Layout: lexer/parser/typechecker/optimizer under `src/`, runtime under `src/runtime/`, headers under `include/erelang/`. CLI is `src/obs_main.cpp` (`erelang`).

To add an import-gated builtin:

1. Implement dispatch in `src/runtime/builtins/<mod>.cpp`.
2. Register it in `src/runtime/builtins.cpp` and aliases in `src/runtime/imports.cpp`.
3. Add the source to `CMakeLists.txt` and a signature in `src/typechecker.cpp`.
4. Document it under `docs/` and add an example if the surface is new.

Type mismatches are errors. `any` is opt-in only.

Keep PRs small. Update docs when you add, remove, or rename a builtin.
