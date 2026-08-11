# Imports and modules

## Builtin modules

Builtin modules have no `.elan` file on disk: they are built into the runtime.

Use the `#include` preprocessor form:

```elan
#include <builtin/fs> as fs
#include <builtin/path> as path
#include <builtin/network> as net
```

The `as alias` part is required for method-call syntax (`fs.read(...)`, `net.get(...)`). Without an alias you can still call the raw builtin names after importing, but method syntax won't work.

Both forms are accepted by the runtime:

```elan
#include <builtin/fs> as fs    // preferred
import "builtin/fs" as fs      // also works
```

## Builtin module list

| Module | Alias | Doc |
|--------|-------|-----|
| `builtin/fs` | `fs` | [filesystem.md](filesystem.md) |
| `builtin/path` | `path` | [filesystem.md](filesystem.md): paths only |
| `builtin/network` | `net` | [network.md](network.md) |
| `builtin/math` | `math` | [math.md](math.md) |
| `builtin/system` | `sys` | [process.md](process.md) |
| `builtin/data` | `db` | [data.md](data.md) |
| `builtin/crypto` | `crypto` | [crypto.md](crypto.md) |
| `builtin/regex` | `re` | [regex.md](regex.md) |
| `builtin/binary` | `bin` | [binary.md](binary.md) |
| `builtin/perm` | `perm` | [permissions.md](permissions.md) |
| `builtin/threads` | `th` | [threads.md](threads.md) |
| `builtin/monitor` | `mon` | [monitor.md](monitor.md) |

`builtin/erefs` and `builtin/erepath` are aliases for `builtin/fs` and `builtin/path`.

## Typechecker

Builtin functions are only registered when their module is imported. Calling `read_text` without importing `builtin/fs` produces `TC001`. See [diagnostics.md](diagnostics.md).

## Method syntax

After `#include <builtin/fs> as fs`, call `fs.read(path)` instead of `read_text(path)`. The runtime resolves aliases at call time: see `src/runtime/imports.cpp` for the full map.

## Local `.elan` modules

Include another `.elan` file to merge its `public action`s into your program:

```elan
@erelang
#include <examples/modules/math.elan>

public action main {
    print sum(10, 5);
    print clamp(150, 0, 100);
}

run main;
```

Paths are relative to the importing file. Unused included actions warn with `TC130`.

## Import resolution

- Paths relative to the importing file
- `builtin/*` resolves internally: no file load
- Imports are loaded recursively before the main program runs
