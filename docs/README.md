# Erelang documentation

Scripting language + Windows-first interpreter. Programs use the `.elan` extension.

## Start here

| Doc | What it covers |
|-----|----------------|
| [getting-started.md](getting-started.md) | Build, run, first script |
| [language.md](language.md) | Actions, types, control flow, entities |
| [imports.md](imports.md) | `#include`, local modules, module list |
| [automation.md](automation.md) | File, process, HTTP scripting patterns |
| [diagnostics.md](diagnostics.md) | Typechecker codes (`TC001`, ...) |

## Builtin modules (import required)

| Doc | Module id |
|-----|-----------|
| [filesystem.md](filesystem.md) | `builtin/fs`, `builtin/path` |
| [process.md](process.md) | `exec` / `spawn` (core) + `builtin/system` |
| [network.md](network.md) | `builtin/network` |
| [collections.md](collections.md) | `list_*`, `dict_*`, `Array`, `Map` |
| [strings.md](strings.md) | `string.*`, interpolation |
| [math.md](math.md) | `builtin/math` |
| [data.md](data.md) | `builtin/data` |
| [crypto.md](crypto.md) | `builtin/crypto` |
| [regex.md](regex.md) | `builtin/regex` |
| [binary.md](binary.md) | `builtin/binary` |
| [threads.md](threads.md) | `builtin/threads` (experimental) |
| [monitor.md](monitor.md) | `builtin/monitor` (experimental) |
| [permissions.md](permissions.md) | `builtin/perm` |

## Core runtime (no import needed)

| Doc | |
|-----|--|
| [core-builtins.md](core-builtins.md) | Time, env, args, conversions, collections, plugin queries |
| [low-level.md](low-level.md) | File handles, pointers, string buffers |

## Tooling

| Doc | |
|-----|--|
| [toolchain.md](toolchain.md) | `erelang`, `obc`, CMake targets, CLI flags |

## Runtime & language reviews

| Doc | |
|-----|--|
| [runtime-review.md](runtime-review.md) | Severity-ordered runtime findings (P0/P1/P2) and suggested improvements |
| [language-gaps.md](language-gaps.md) | Missing language features with implemented-vs-missing status |

## Examples in repo

- `examples/program.elan`: beginner hello-world demo
- `examples/language_kitchen_sink.elan`: entities, structs, collections, control flow
- `examples/modules/math.elan`: reusable action library
- `examples/testfile.elan`: CLI-tool capability demo (arg parsing, subprocess capture, file I/O)
