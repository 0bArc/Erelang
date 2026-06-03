# Permissions (in-memory)

Script-level permission registry — not OS ACLs. Useful for gating features inside long-running hosts or plugin systems.

```elan
#include <builtin/perm> as perm
```

| Builtin | Args | Returns |
|---------|------|---------|
| `perm_grant(name)` | permission name | void |
| `perm_revoke(name)` | permission name | void |
| `perm_has(name)` | permission name | `"true"` / `"false"` |
| `perm_list()` | — | list handle |

## Example

```elan
@erelang
#include <builtin/perm> as perm

public action main {
    perm_grant("network.download")
    if (perm_has("network.download") == "true") {
        print "allowed"
    }
}

run main;
```

## Related

- [imports.md](imports.md)
