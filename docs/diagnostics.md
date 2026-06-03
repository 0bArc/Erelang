# Diagnostics

The typechecker emits `TC***` codes before the script runs. **Errors** stop execution. **Warnings** still run.

## Error codes

| Code | Meaning |
|------|---------|
| `TC001` | Unknown action or builtin — missing import, or typo |
| `TC010` | Variable used before declaration |
| `TC020` / `TC021` | Parameter count mismatch |
| `TC030` | Variable redeclared in same scope |
| `TC100`+ | Duplicate action / entity / struct name |
| `TC110` | `run` target not found |
| `TC111` | No `run` directive and no `main` action |
| `TC121` | Non-void action missing `return` |

## Warning codes

| Code | Meaning |
|------|---------|
| `TC070` | Unreachable code after `return` |
| `TC120` | Unused variable |
| `TC130` | Unused action |
| `TC131` | Unused entity method |
| `TC132` | Unused entity |

## Fixing TC001 — unknown builtin

```
[error] TC001: Unknown action: file_open (main)
```

1. Add the import: `#include <builtin/fs> as fs`
2. Use the alias: `fs.read(path)`, `fs.write(path, data)`
3. Or use the raw builtin name **after** importing: `read_text`, `write_text`

Every builtin-gated function requires its module to be imported. See [imports.md](imports.md) for the full module list.

## Fixing TC001 — typo in action name

Check the name you're calling matches the declared `public action` name exactly (case-sensitive).

## Fixing TC130 — unused included action

`#include <modules/math.elan>` merges every `public action`. Either call them or remove the include.

## Fixing TC111 — no entry point

Every runnable file needs:

```elan
run main;
```

at the bottom, where `main` is the name of a `public action`.

## Related

- [imports.md](imports.md)
- [language.md](language.md)
- [core-builtins.md](core-builtins.md)
