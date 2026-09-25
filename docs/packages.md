# Packages

Erelang packages are `.elan` projects, not npm or Cargo crates.

## Manifest (`package.elan`)

A package root contains `package.elan` the normal parser can read. Metadata lives in the `pkg` namespace as globals:

```elan
@erelang

namespace pkg {
    global string name = "greet";
    global string version = "1.0.0";
    global string entry = "lib.elan";
}

namespace pkg.depends {
    global string util = "1.0.0";
    global string other = "^1.2.0";
}
```

- `name` / `version` — required
- `entry` — default `lib.elan`; file loaded for `#include <pkg/NAME>`
- `pkg.depends` — dependency name = requirement (`1.0.0` exact, or `^1.0.0` compatible same major)

## Local registry

Directory layout:

```
registry/
  greet/
    1.0.0/
      package.elan
      lib.elan
  util/
    1.0.0/
      package.elan
      lib.elan
```

Set `ERELANG_REGISTRY` or pass `--registry <dir>`.

## Lockfile (`erelang.lock`)

Exact version plus content hash (FNV-1a over sorted file paths+bytes):

```
# erelang.lock — exact version + content hash
greet 1.0.0 a1b2c3d4e5f6 registry/greet/1.0.0
util 1.0.0 9f8e7d6c5b4a registry/util/1.0.0
```

## CLI

```bash
erelang --lock package.elan --registry ./registry
erelang --fetch package.elan --registry ./registry
erelang --fetch package.elan --registry ./registry --cache .erelang/pkgs
```

`--lock` resolves depends and writes `erelang.lock` next to the manifest.  
`--fetch` copies locked trees into `.erelang/pkgs/<name>/<version>/` (or `--cache`).

## Importing a package

```elan
#include <pkg/greet>
```

Resolution:

1. Walk parents of the importing file for `erelang.lock`
2. Look up the package name
3. Prefer `.erelang/pkgs/<name>/<version>/`, else lock path / registry
4. Load the package `entry` file (or `#include <pkg/greet/other.elan>` for a subpath)

This extends the existing `#include` / `import` module system — there is no second module graph.

## Fixture

See `tests/packages/` for a tiny local registry and app that locks, fetches, and `#include <pkg/greet>`.

## Related

- [imports.md](imports.md)
- [toolchain.md](toolchain.md)
