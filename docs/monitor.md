# File monitor

Poll files for size, mtime, or content hash changes. Requires `-DERELANG_EXPERIMENTAL=ON`.

```elan
#include <builtin/monitor> as mon
```

| Builtin | Args | Returns |
|---------|------|---------|
| `monitor_add(path, key?)` | path, optional name/key for the monitor | monitor id |
| `monitor_remove(id)` | | void |
| `monitor_list()` | | list of active monitor ids |
| `monitor_info(id)` | | metadata string |
| `monitor_last_change(id)` | | change description string, or empty |
| `monitor_set_interval(id, ms)` | id, poll interval in ms | void |

## Example

```elan
@erelang
#include <builtin/monitor> as mon

public action main {
    id = monitor_add("config.ini");
    monitor_set_interval(id, 1000);
    print "watching config.ini...";

    sleep 5000ms;

    change = monitor_last_change(id);
    if (change == "") {
        print "no changes detected";
    } else {
        print "changed: {change}";
    }

    monitor_remove(id);
}

run main;
```

## Notes

- Polling runs in native code; the script queries results via `monitor_last_change`.
- Default poll interval is 2000ms. Change it with `monitor_set_interval`.
- Pair with threads ([threads.md](threads.md)) for reactive background watching.

## Related

- [filesystem.md](filesystem.md)
- [threads.md](threads.md)
- [automation.md](automation.md)
