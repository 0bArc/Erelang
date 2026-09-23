# Diagnostics

The typechecker emits `TC***` codes before the script runs. **Errors** stop execution. **Warnings** still run.

## Error codes

| Code | Meaning |
|------|---------|
| `TC001` | Unknown action or builtin: missing import, or typo |
| `TC010` | Variable used before declaration |
| `TC020` / `TC021` | Parameter count mismatch |
| `TC022` | Argument type mismatch |
| `TC030` | Variable redeclared in same scope |
| `TC100`+ | Duplicate action / entity / struct name |
| `TC110` | `run` target not found |
| `TC111` | No `run` directive and no `main` action |
| `TC121` | Non-void action missing `return` |
| `TC140` | Opaque type parameter used in operator without a trait constraint |
| `TC141` | Cannot infer type parameter; pass explicit `<...>` args |
| `TC142` | Unknown trait constraint |
| `TC143` | Concrete type does not satisfy trait (missing method) |
| `TC144` | Type argument count mismatch |
| `TC145` | Match binding count mismatch for variant |
| `TC146` | Enum variant payload arity mismatch |
| `TC147` | Method not provided by type-parameter constraints |
| `TC148` | Match used on non-enum value |
| `TC149` | Pattern variant used where a different enum type was expected |
| `TC150` | Unknown enum variant (construction or pattern) |
| `TC151` | Enum variant payload type mismatch |
| `TC153` | Unknown field in struct/entity destructuring pattern |
| `TC154` | Struct pattern used on non-struct/entity value |
| `TC155` | Array pattern used on non-array value |
| `TC156` | Pattern form used in the wrong context (`let` vs `match`) |
| `TC157` | Tuple arity mismatch (literal or destructuring) |
| `TC158` | `?` used on non-Result/Option or incomplete type |
| `TC159` | `?` Option/Result or error-type incompatibility with function return |
| `TC160` | `await` used outside an `async` action |
| `TC161` | Non-exhaustive `match` on an enum (missing variants; use `_` or cover all) |
| `TC162` | Match guard expression is not `bool` |
| `TC163` | Range `..` / `..<` bounds are not `int` |

## Warning codes

| Code | Meaning |
|------|---------|
| `TC070` | Unreachable code after `return` |
| `TC120` | Unused variable |
| `TC130` | Unused action |
| `TC131` | Unused entity method |
| `TC132` | Unused entity |

## Fixing TC001: unknown builtin

```
[error] TC001: Unknown action: file_open (main)
```

1. Add the import: `#include <builtin/fs> as fs`
2. Use the alias: `fs.read(path)`, `fs.write(path, data)`
3. Or use the raw builtin name **after** importing: `read_text`, `write_text`

Every builtin-gated function requires its module to be imported. See [imports.md](imports.md) for the full module list.

## Fixing TC001: typo in action name

Check the name you're calling matches the declared `public action` name exactly (case-sensitive).

`new Counter()` is entity construction, not an action call. The typechecker and language server treat the name after `new` as an entity type. A red underline on `Counter()` means the entity is not declared in this file — not that `new` is invalid.

## Fixing TC130: unused included action

`#include <modules/math.elan>` merges every `public action`. Either call them or remove the include.

## Fixing TC111: no entry point

Every runnable file needs:

```elan
run main;
```

at the bottom, where `main` is the name of a `public action`.

## Related

- [imports.md](imports.md)
- [language.md](language.md)
- [core-builtins.md](core-builtins.md)
