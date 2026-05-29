# Experimental / optional modules

## Build flag

`-DERELANG_EXPERIMENTAL=ON` enables `builtin/threads` and `builtin/monitor` implementations.

## Import-gated builtins

Not global. Import then call (direct or via alias):

```elan
import builtin/network as net
import builtin/math as math
import builtin/data as data
import builtin/perm as perm
import builtin/system as sys
import builtin/fs as fs

public action main() {
  let html = net.http_get("https://example.com")
  let sum = math.add(2, 3)
  let store = data.data_new()
}
```

| Import path | Examples |
|-------------|----------|
| `builtin/fs` / `builtin/path` | `read_text`, `path_join`, … |
| `builtin/network` | `http_get`, `network.ip.*` |
| `builtin/regex` | `regex_match`, `re.match` |
| `builtin/crypto` | `hash_fnv1a`, `random_bytes` |
| `builtin/binary` | `bin_new`, `bin_hex`, … |
| `builtin/math` | `add`, `sqrt`, `collatz_len`, … |
| `builtin/data` | `data_new`, `data_set`, `data_get`, … |
| `builtin/perm` | `perm_grant`, `perm_has`, … |
| `builtin/system` | `system.cmd`, `system.execute`, … |
| `builtin/threads` | `thread_run`, … (experimental build) |
| `builtin/monitor` | `monitor_add`, … (experimental build) |

## Core globals (~40 names)

No import: `now_ms`, `env`, `rand_int`, `uuid`, `args_*`, `read_line`, `input`, `exec`/`spawn`/`exit`, conversions, basic `string.*`, `list_*`, `dict_*`, `plugin_core*`, `language_name`/`language_version`.

Removed from core (import or delete): `machine_guid`, `hwid`, `volume_serial`, `username`, `computer_name`, GUI/win builtins, ptr/malloc, tables/sets/queues, file handle API duplicates.
