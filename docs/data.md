# Data stores

In-memory key/value store with optional disk persistence. Good for saving app state between runs.

```elan
#include <builtin/data> as db
```

| Builtin | Args | Returns |
|---------|------|---------|
| `data_new()` | | store handle |
| `data_set(store, key, value)` | key, value | void |
| `data_get(store, key)` | key | value string |
| `data_has(store, key)` | key | `"true"` / `"false"` |
| `data_keys(store)` | | key list |
| `data_save(store, path)` | | writes to file |
| `data_load(path)` | | loads from file (empty store if missing) |

## Example: run counter

```elan
@erelang
#include <builtin/data> as db

public action main {
    let s = data_load("state.txt");
    if (data_has(s, "count") == "false") {
        data_set(s, "count", "0");
    }
    let n = toint(data_get(s, "count")) + 1;
    data_set(s, "count", tostr(n));
    data_save(s, "state.txt");
    print "run #{n}";
}

run main;
```

## `dict_*` vs `data_*`

| | `dict` literals | `data_*` |
|--|----------|----------|
| Scope | in-memory map | flat key/value store |
| Persist | manual via `fs.write` | `data_save` / `data_load` |
| Use | in-script structures | app state across runs |

## Related

- [collections.md](collections.md)
- [filesystem.md](filesystem.md)
- [automation.md](automation.md)
