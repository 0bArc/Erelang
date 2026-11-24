---
name: build-and-test
description: How to configure, build, run and test Erelang on Windows, Linux and macOS (CMake presets, binary paths, conformance suite, plugin tests, benchmarks, CI, VS Code extension tests, build options and release rules). Use before building, running scripts, running tests, or touching CMake, CI or release configuration.
---

# Build and test

Run everything from the repository root. Windows uses MinGW (`mingw32-make`, `g++`). Linux and macOS use the system compiler with Unix Makefiles and need libcurl and OpenSSL (Linux: `libcurl4-openssl-dev libssl-dev`; macOS: `brew install openssl@3`, found automatically).

## Build

| OS | Debug | Release |
| --- | --- | --- |
| Windows | `cmake --preset debug` then `cmake --build --preset debug -j 8` | `release` |
| Linux, macOS | `cmake --preset unix-debug` then `cmake --build --preset unix-debug -j 8` | `unix-release` |

| Build | Interpreter |
| --- | --- |
| debug | `build/debug/bin/Debug/erelang` (`erelang.exe` on Windows) |
| release | `build/release/bin/Release/erelang` (`erelang.exe` on Windows) |

- The Windows debug build puts `build/debug/bin/Debug` first on the user PATH (`ERELANG_UPDATE_PATH=ON`). Release and the Unix presets leave PATH alone. `tools/fix-erelang-path.ps1` repairs PATH by hand on Windows.
- Do not create other build folders. Do not delete or rebuild a build folder while a dev server, the extension's language server, or another `erelang` from it is running.
- After adding a source file, add it to `CMakeLists.txt` for every library configuration that needs it, then re-run the configure preset.
- Add `--target erelang_runner` to build only the interpreter.

## Run

```powershell
./build/debug/bin/Debug/erelang examples/program.elan
./build/debug/bin/Debug/erelang --check examples/program.elan
```

## Tests

The scripts are PowerShell and run under `pwsh` 7 on every OS (Windows PowerShell 5.1 also works for `tests/run.ps1`).

```powershell
pwsh tests/run.ps1                                   # debug build of this OS; -Exe <path> to override
pwsh plugins/discord/tests/run.ps1 -Exe <erelang>    # mock REST + gateway server (needs Python 3)
pwsh plugins/docker/tests/run.ps1 -Exe <erelang>     # fake docker CLI (docker.cmd / docker)
python tools/check_builtin_registry.py               # registry vs manifest
```

Read `tests/README.md` before changing tests. Positive `*.elan` tests must exit zero. Negative `*_neg.elan` tests must exit nonzero; an optional `// expect: ...` marker checks diagnostic output. Add a regression near related tests and exercise both normal and invalid inputs for new semantics. Run relevant examples as well as the suite.

Tests that run shell commands must work on every OS: branch on `sys.os()` (`"windows"`, `"linux"`, `"macos"`) instead of hardcoding `cmd.exe`, `findstr` or `ping -n`.

## Cross-platform code

- OS-specific code lives behind `#ifdef _WIN32` / `__APPLE__` with a working branch for every platform. A stub that returns empty or `false` is not a port.
- Sockets go through `src/runtime/builtins/net_platform.hpp` (WinSock names on every OS, `set_socket_timeout`, `tcp_connect`).
- HTTP: WinHTTP on Windows, libcurl elsewhere, both behind `http_request` in `network.cpp`. WebSocket client: WinHTTP on Windows, socket + OpenSSL elsewhere. AES-GCM and random bytes: BCrypt on Windows, OpenSSL elsewhere. The AES ciphertext format is the same everywhere.
- `--build-native` and `--emit-asm` produce Windows x64 code; `--build-native` refuses to run on other platforms.

## Linux check without a Linux machine

Use a throwaway container from an image already on the machine (`docker images`, for example `gcc:13`): mount the repo read-only, build in a folder inside the container, copy the sources to a writable folder for tests that write files, and remove the container afterwards. Do not pull new images or download tools into it without asking.

## CI

`.github/workflows/ci.yml` builds the `unix-debug` preset and runs the example, the conformance suite and both plugin tests on `ubuntu-latest` and `macos-latest`. Windows is built and tested locally.

## Benchmarks

```powershell
pwsh examples/bench/run.ps1                          # uses the release build
```

Compare against a baseline from the same machine before claiming a speedup.

## VS Code extension

```powershell
cd erevos-language
npm ci
npm test
```

`tools/gen-extension-manifest.ps1` regenerates `erevos-language/resources/builtins.json` from the built interpreter; run it after adding or changing a builtin. Do not package or commit VSIX files unless requested. An internal C++ cleanup without an editor-visible change does not require rebuilding the extension. If extension syntax, diagnostics, or completion behavior changes, verify those areas with tests.

## Build configuration and releases

`erelang` is the static library, `erelang_runner` produces the interpreter, and `obc` is the compiler CLI. Options live at the top of `CMakeLists.txt` (`BUILD_SHARED_RUNTIME`, `ERELANG_EXPERIMENTAL`, `ERELANG_UPDATE_PATH`, ...). Review all relevant targets when adding source files. Check actual CMake values rather than relying on older descriptions in docs.

Do not change version numbers, release metadata, packaged extensions, or other release artifacts without explicit approval. Do not modify unrelated submodules (`tools/symcheck`) or generated files. Preserve unrelated working-tree changes.
