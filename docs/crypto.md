# Crypto helpers

```elan
#include <builtin/crypto> as crypto
```

| Alias | Builtin | Args | Returns |
|-------|---------|------|---------|
| `crypto.hash(s)` | `hash_fnv1a` | string | 16-char hex hash string (FNV-1a 64-bit) |
| `crypto.random_bytes(n)` | `random_bytes` | byte count | hex string |

Not a full cryptography library — provides fast hashing and random bytes for scripting utilities. `hash` uses FNV-1a (fast, not cryptographically secure).

## Example

```elan
@erelang
#include <builtin/crypto> as crypto

public action main {
    let h = crypto.hash("hello world");
    print "hash={h}";

    let rb = crypto.random_bytes(16);
    print "random={rb}";
}

run main;
```

## Related

- [binary.md](binary.md)
- [core-builtins.md](core-builtins.md)
