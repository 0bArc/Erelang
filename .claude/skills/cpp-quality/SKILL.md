---
name: cpp-quality
description: C++20 style and correctness rules for Erelang's interpreter, runtime and compiler (ownership, lifetimes, errors, concurrency, permissions, optimizations). Use for any edit to C++ in src/, include/ or experimental/.
---

# C++ quality

## Style

- Use C++20, four-space indentation, and braces on the same line, matching existing file style.
- Prefer clear, local solutions. Do not add extra layers, wrapper classes, or generic helpers without a demonstrated need.
- Follow existing naming and error-handling conventions. Do not reformat or rename unrelated code.
- Prefer RAII, explicit ownership, const correctness, and standard-library solutions where appropriate.
- Use `std::string_view` only when its referenced storage remains alive. Avoid dangling references, unnecessary copies, and unexpected allocations.
- Prefer scoped state to mutable globals. Keep public headers and interfaces narrow.
- Comments explain non-obvious intent, invariants, ownership, or compatibility requirements, not obvious statements.
- Do not add unused includes, commented-out code, placeholder implementations, silent catch-all fallbacks, or TODOs as substitutes for finishing work.
- Do not hide a failure by returning an empty string, null result, or success code unless that is the documented semantic contract.
- Do not introduce dependencies, refactors, feature flags, or public APIs unrelated to the request.
- Avoid introducing new compiler warnings. Never suppress errors or weaken type checking just to make tests pass.
- Preserve existing valid syntax and API behavior unless a breaking change is explicitly requested.

## Platforms

- Code must build and behave the same on Windows (MinGW), Linux (GCC) and macOS (Clang). Give every `#ifdef _WIN32` a real POSIX branch; see `build-and-test` for the platform layers.
- Strings crossing an OS API are UTF-8 on POSIX. Do not narrow `wchar_t` by casting.
- POSIX child processes: mark the parent's pipe ends close-on-exec. Ignore `SIGPIPE` before writing to sockets (`net::ensure_init`).

## Runtime and compiler correctness

- Preserve ownership, move/copy behavior, allocation and release contracts, and value lifetimes.
- For concurrency or async work, consider cancellation, exception propagation, task lifetimes, synchronization, and deadlocks.
- Respect import and permission boundaries for filesystem, process, networking, and other privileged capabilities.
- Reject malformed input safely and deliberately. Do not mask runtime errors.
- Validate transformations against observable behavior. For optimizations, use relevant tests and representative `examples/bench/` cases rather than assuming they are faster.
