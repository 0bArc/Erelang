# Erelang (Erevos Language)

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Build](https://img.shields.io/badge/build-CMake-orange)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![GitHub Stars](https://img.shields.io/github/stars/0bArc/Erelang)
![GitHub Forks](https://img.shields.io/github/forks/0bArc/Erelang)
![GitHub Issues](https://img.shields.io/github/issues/0bArc/Erelang)
![GitHub Last Commit](https://img.shields.io/github/last-commit/0bArc/Erelang)
![Repo Size](https://img.shields.io/github/repo-size/0bArc/Erelang)
![License](https://img.shields.io/github/license/0bArc/Erelang)

---

## Overview

**Erelang** (Erevos Language) is a modern, high-performance programming language designed for developers who demand control, speed, and clarity. Built with **C++17** and powered by a robust CMake build system, Erelang aims to deliver a clean syntax, efficient execution, and a productive development experience.

## Features

- **Blazing Fast:** Compiled with C++17 for minimal overhead and maximum performance.
- **Modern Syntax:** Clean, expressive language design inspired by contemporary programming paradigms.
- **Cross-Platform Build:** CMake-based system supports MSVC, MinGW, and Clang (Windows-focused).
- **Lightweight & Modular:** Minimal dependencies, easy to clone, build, and extend.
- **Extensible:** Designed for future integration with custom VMs or LLVM backends.

## Getting Started

### Prerequisites

- **Compiler:** MSVC (Visual Studio 2019+), MinGW, or Clang with C++17 support
- **Build System:** CMake 3.15 or newer

### Build Instructions

```bash
# Clone the repository
$ git clone https://github.com/0bArc/Erelang.git
$ cd Erelang

# Configure and build (Ninja example)
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
$ cmake --build build -j 8

# Or use MinGW/Visual Studio as needed
```

### Running Erelang Code

```bash
# Run an Erelang source file
$ ./build/erelang.exe examples/test.elan
```

## Roadmap

- [x] Core Lexer and Parser
- [x] Abstract Syntax Tree (AST)
- [x] Standard data types & control flow
- [ ] Advanced type system
- [ ] Custom Virtual Machine / LLVM backend
- [ ] Standard library & modules

## Code Syntax
**Checkfile**
<img width="600" height="392" alt="image" src="https://github.com/user-attachments/assets/0456d6ae-aea2-4783-b7a7-7fe4911c7a2a" />


## Contributing

Contributions are welcome! Please open an **Issue** for bugs/feature requests or submit a **Pull Request**. See [CONTRIBUTING.md] if available.

## License

This project is licensed under the Appache License. See the [LICENSE](LICENSE) file for details.
