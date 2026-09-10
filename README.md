# Erelang

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&amp;logo=cplusplus&amp;logoColor=white" />
  <img alt="CMake" src="https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&amp;logo=cmake&amp;logoColor=white" />
  <img alt="Windows" src="https://img.shields.io/badge/platform-Windows-0078D4?style=flat-square&amp;logo=windows&amp;logoColor=white" />
  <img alt="Version" src="https://img.shields.io/badge/version-0.0.1-7c3aed?style=flat-square" />
  <img alt="License" src="https://img.shields.io/github/license/0bArc/Erelang?style=flat-square" />
  <img alt="Last commit" src="https://img.shields.io/github/last-commit/0bArc/Erelang?style=flat-square" />
</p>

Erelang is a scripting language with its own interpreter (`erelang.exe`). You write `.elan` files: actions, entities, enums, hooks, then `run` an entry point. The runtime is C++20 and targets Windows.

It exists because the usual options were a bad fit. Python and JavaScript pull in a whole ecosystem for a few scripts. Embedding Lua or a custom DSL in C++ still meant fighting someone else's syntax and object model. Erelang is the language and the VM in one tree. You can change both.

Scripts stay small on purpose. There is no GUI in the core. Filesystem, network, math, and the rest are `#include` modules, so a hello-world does not load HTTP. Entities and hooks are there for game-shaped programs (objects with methods, work that runs around `main`) without turning the language into C++.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target erelang_runner -j 8
./build/bin/Debug/erelang.exe examples/program.elan
```

`-DERELANG_EXPERIMENTAL=ON` enables `builtin/threads` and `builtin/monitor`.

## Entity

![Entity](docs/entity.svg)

## Switch

![Switch](docs/switch.svg)

## Collections

![Collections](docs/collections.svg)

## Filesystem and hooks

![Filesystem and hooks](docs/fs.svg)

## Include

![Include](docs/include.svg)

Docs: [docs/](docs/README.md). Editor: `erevos-language/`. License: [Apache-2.0](LICENSE).
