# Docs

| Doc | |
|-----|--|
| [getting-started.md](getting-started.md) | Build and run |
| [language.md](language.md) | Actions, types, control flow, entities |
| [imports.md](imports.md) | `#include` and builtin modules |
| [diagnostics.md](diagnostics.md) | Typechecker codes |
| [toolchain.md](toolchain.md) | `erelang`, `obc`, CMake flags |

## Builtin modules

| Doc | Import |
|-----|--------|
| [filesystem.md](filesystem.md) | `builtin/fs`, `builtin/path` |
| [process.md](process.md) | `exec` / `spawn` (core), `builtin/system` |
| [network.md](network.md) | `builtin/network` |
| [collections.md](collections.md) | lists, dicts, `Array`, `Map` |
| [strings.md](strings.md) | `string.*`, interpolation |
| [math.md](math.md) | `builtin/math` |
| [data.md](data.md) | `builtin/data` |
| [crypto.md](crypto.md) | `builtin/crypto` |
| [regex.md](regex.md) | `builtin/regex` |
| [binary.md](binary.md) | `builtin/binary` |
| [threads.md](threads.md) | `builtin/threads` (experimental) |
| [monitor.md](monitor.md) | `builtin/monitor` (experimental) |
| [permissions.md](permissions.md) | `builtin/perm` |
| [core-builtins.md](core-builtins.md) | no import |
| [low-level.md](low-level.md) | file handles, pointers, string buffers |
| [automation.md](automation.md) | file / process / HTTP patterns |

## Examples

- `examples/program.elan` — small run target (includes the debugger lib)
- `examples/language_kitchen_sink.elan` — language surface
- `examples/modules/math.elan` — included actions
