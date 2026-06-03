# Threads

Requires the experimental build flag: `-DERELANG_EXPERIMENTAL=ON`.

```elan
#include <builtin/threads> as th
```

| Builtin | Args | Returns |
|---------|------|---------|
| `thread_run(actionName)` | name of a `public action` | thread id string |
| `thread_join(id)` | | `"true"` when finished |
| `thread_join_timeout(id, ms)` | | `"true"` if finished within timeout |
| `thread_done(id)` | | `"true"` / `"false"` |
| `thread_state(id)` | | status string |
| `thread_count()` | | active thread count |
| `thread_wait_all()` | | waits for all threads |
| `thread_yield()` | | yield to scheduler |
| `thread_gc()` | | clean up finished threads |
| `thread_remove(id)` | | remove a thread record |

## Example

```elan
@erelang
#include <builtin/threads> as th

public action worker {
    sleep 500ms;
    print "worker done";
}

public action main {
    let id = thread_run("worker");
    print "started thread={id}";
    thread_join(id);
    print "all done";
}

run main;
```

## Notes

- `thread_run` takes the **name** of an action as a string, not the action itself.
- Use `thread_wait_all()` to wait for all active threads before exiting.
- Threads share global variables — coordinate access carefully.

## Related

- [monitor.md](monitor.md)
- [automation.md](automation.md)
