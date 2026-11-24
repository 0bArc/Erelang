# 2026-11-23 Linux and macOS port

## What was wrong

Erelang only built on Windows.

- `src/runtime/builtins/network.cpp` used WinHTTP for the HTTP client and WinSock types (`SOCKET`, `closesocket`, `DWORD` timeouts, `SetConsoleCtrlHandler`, BCrypt SHA-1) outside any `_WIN32` guard. 644 compile errors on Linux. The guarded part had empty stubs (`http_get` returned "", `network.ip.*` "unsupported platform").
- `src/runtime/builtins/websocket.cpp`: WinHTTP WebSocket client; on POSIX `ws_connect` returned -1 and the dispatch still called `WinHttpWebSocketSend`.
- `src/runtime/builtins/crypto.cpp`: `aes_encrypt` / `aes_decrypt` threw "not supported on this platform"; `random_bytes` used `std::random_device`.
- `src/runtime/builtins/system.cpp`: POSIX arguments went through `wcstombs` (C locale) or a `wchar_t` to `char` cast, so non-ASCII arguments broke; pipe ends leaked into later children; `ip.flush` returned false.
- `src/runtime/builtins.cpp`: `exec` returned the raw wait status (exit code times 256); `run_file` / `run_bat` did nothing; `uuid` was not a valid version 4 UUID.
- `src/modules.cpp`: `.odll` modules only loaded on Windows.
- `src/obs_main.cpp`: no executable path lookup on POSIX; `--compile` generated a CMake project linking Windows libraries with a MinGW generator and `app.exe`; the 64 MiB stack came from `-Wl,--stack`, which does not exist on Linux or macOS.
- `CMakeLists.txt`: `-Wl,--stack` and GNU-only `--gc-sections` / `--strip-all` on every non-MSVC build; no HTTP/TLS libraries for POSIX.
- `tests/std/process_json_math.elan` hardcoded `cmd.exe`, `ping -n` and `findstr`. Scripts had no way to ask which OS they run on.
- Test and tool scripts used backslash paths and `erelang.exe`; the Docker plugin test only had `docker.cmd`.

## What changed

- New `src/runtime/builtins/net_platform.hpp` / `.cpp`: WinSock names on POSIX (`SOCKET`, `INVALID_SOCKET`, `closesocket`, `WSAGetLastError`), `net::ensure_init` (WSAStartup, or ignore SIGPIPE), `set_socket_timeout`, `tcp_connect` (getaddrinfo, connect timeout), portable SHA-1, base64 and `ws_accept_key` (replaces the BCrypt version).
- `network.cpp`: one `http_request(method, url, headers, body, contentType, sink)` per platform (WinHTTP; libcurl with the same timeouts, redirects, 64 MiB cap and no `Expect: 100-continue`). Every `http_*` builtin is now a shared wrapper over it, which removed about ten near-identical WinHTTP functions. HLS helpers are shared. POSIX `execute_cmd` uses `posix_spawnp`. `network.ip.*` maps to `ipconfig`, `resolvectl` / `dhclient`, or `dscacheutil` / `ipconfig set`. Ctrl+C on POSIX sets an atomic flag that the accept loop polls (`SA_RESETHAND`, so a second Ctrl+C kills). `select` gets the right first argument. `gmtime_r`, `socklen_t`.
- `websocket.cpp`: shared connection registry, inbox, close handling and dispatch; Windows keeps WinHTTP. POSIX speaks RFC 6455 over a socket with optional OpenSSL TLS (TLS 1.2+, peer and hostname verification, SNI), masks client frames, answers pings, joins fragments, checks `Sec-WebSocket-Accept`. One `ws_send_message` replaces four copies of the send code; fixed a reference leak in `ws_send_binary` when the socket was closed.
- `crypto.cpp`: OpenSSL EVP AES-256-GCM and `RAND_bytes` on POSIX. Ciphertext layout (nonce | cipher | tag) is identical, so data moves between platforms.
- `system.cpp`: UTF-8 widen/narrow on POSIX, close-on-exec parent pipe ends, `ip.flush` per platform, new `system.os` builtin.
- `builtin_registry.cpp`: `sys.os()` / `proc.os()` (`"windows"`, `"linux"`, `"macos"`); `ip.flush` description no longer says Windows.
- `builtins.cpp`: `exec` returns the exit code; `run_file` uses `open` / `xdg-open`; `run_bat` runs `sh` on POSIX; proper v4 UUID.
- `modules.cpp`: `.odll` discovery through `std::filesystem`, loading through `LoadLibraryW` or `dlopen`.
- `obs_main.cpp`: executable path from `/proc/self/exe` or `_NSGetExecutablePath`; on POSIX `main` runs the interpreter on a 64 MiB pthread; `--compile` generates `if(WIN32)` link lines (curl, OpenSSL, threads, dl elsewhere), picks Unix Makefiles or Ninja, and drops `.exe`; `--build-native` (Win64 code) refuses with a clear message off Windows. Same refusal in `main.cpp` (`obc`).
- `CMakeLists.txt`: `net_platform.cpp` in both library lists; POSIX links `CURL::libcurl`, `OpenSSL::SSL`, `OpenSSL::Crypto`, `Threads::Threads`, `${CMAKE_DL_LIBS}`; Homebrew OpenSSL found automatically on macOS; `-Wl,--stack` only on Windows; `-Wl,-dead_strip` on macOS.
- `CMakePresets.json`: `unix-debug` / `unix-release` (Unix Makefiles, same `build/debug` and `build/release` folders); the Windows presets got a host condition.
- Tests and tools: `tests/process_json_math.elan` branches on `sys.os()`; `tests/run.ps1`, `examples/bench/run.ps1`, `plugins/*/tests/run.ps1`, `tools/gen-extension-manifest.ps1`, `tools/check_builtin_registry.py` pick the binary name per OS and use forward slashes; new `plugins/docker/tests/fake/docker` (POSIX twin of `docker.cmd`); `examples/bench/run.ps1` writes its temp script without a BOM (fixes Windows PowerShell 5.1).
- Extension: `erelangPath.ts`, `dap_smoke.test.js`, `completions.test.js` look for `erelang` without `.exe` off Windows; `resources/builtins.json` regenerated.
- CI: `.github/workflows/ci.yml` builds `unix-debug` and runs the example, the suite and both plugin tests on ubuntu-latest and macos-latest. `.gitignore` now ignores `.github/*` except `workflows/`. New `.gitattributes` keeps shell scripts LF.
- Docs: README, CONTRIBUTING, getting-started (prerequisites and per-OS build), process module (`sys.os`, `sys.ip.flush`), toolchain (`--emit-asm` is Win64). `CLAUDE.md`, `build-and-test` and `cpp-quality` skills describe the platform rules.

## Behavior changes

- Linux and macOS builds exist and behave like Windows for HTTP, WebSocket (`ws://` and `wss://`), crypto, processes, sockets and plugins.
- New builtin `sys.os()`.
- `exec` on POSIX returns the exit code instead of the wait status.
- Windows: `http_download` removes a partial file when the request fails; `http_*` bodies above 4 GiB fail instead of being sent; otherwise unchanged.
- `--build-native` exits with an error on Linux and macOS.

## Verification

- Windows: `cmake --preset debug` / `release` and both builds succeed; no new warnings in changed files. `pwsh tests/run.ps1` and Windows PowerShell 5.1 `tests/run.ps1`: pass=91 fail=0. Discord plugin test 9/9, Docker plugin test 6/6. `python tools/check_builtin_registry.py`: exit 0. `npm test` in `erevos-language`: 66/66. `examples/bench/run.ps1` now runs under Windows PowerShell 5.1.
- Linux (throwaway `gcc:13` container: GCC 13, CMake 3.25, libcurl 7.88, OpenSSL 3.0): debug and release build with no errors. Conformance suite through a shell copy of the `run.ps1` rules: all 91 real tests pass. Discord plugin test 7 checks pass (REST through curl, gateway identify, heartbeat, reconnect with resume, rate-limit retry). Docker plugin test 6/6 with the new POSIX fake. AES round trip, `random_bytes`, `uuid`, `sha256`, `https://example.com/` status 200, and `wss://` echo against a local TLS server; connecting by IP to the same server is rejected (hostname mismatch).
- AES ciphertext made on Linux decrypts on Windows and the reverse.

## Not done

- macOS was not built or run here; the first CI run is the first macOS build.
- `tests/run.ps1` itself was not run on Linux (pwsh was not available in the container); CI runs it.
- The new CI workflow has not run yet.
- `http.create_server_tls` ignores its certificate and key on every platform (plain HTTP); unchanged.
- `--build-native` / `--emit-asm` only target the Windows x64 ABI.
- Generated bootstrap apps (`--compile --dynamic`) still load `.odll` files only on Windows.
- `erevos-language/test/compilerInput.test.js` and `server.test.js` (added by another session today) still hardcode `erelang.exe`.
