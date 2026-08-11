# Binary buffers

Growable byte buffers for building binary data or reading hex.

```elan
#include <builtin/binary> as bin
```

| Builtin | Args | Returns |
|---------|------|---------|
| `bin_new()` | | buffer handle |
| `bin_push_u8(buf, byte)` | 0-255 (decimal integer) | void |
| `bin_len(buf)` | | byte count |
| `bin_get_u8(buf, index)` | | byte value (0-255) |
| `bin_hex(buf)` | | lowercase hex string |
| `bin_from_hex(text)` | hex string | new buffer handle |

**Note:** `bin_push_u8` takes a decimal integer (0-255). Hex literals like `0xFF` are not supported: use `255` instead.

## Example

```elan
@erelang
#include <builtin/binary> as bin

public action main {
    b = bin_new();
    bin_push_u8(b, 255);
    bin_push_u8(b, 0);
    bin_push_u8(b, 171);

    print "len={bin_len(b)}";
    print "hex={bin_hex(b)}";
}

run main;
```

## Round-trip from hex

```elan
@erelang
#include <builtin/binary> as bin

public action main {
    b = bin_from_hex("deadbeef");
    print bin_hex(b);
}

run main;
```

## Related

- [crypto.md](crypto.md)
- [low-level.md](low-level.md)
