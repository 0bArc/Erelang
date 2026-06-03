# Low-level APIs

Advanced scripting: streaming file handles, string builders, pointers and memory. Most automation should use the high-level helpers in [filesystem.md](filesystem.md) instead.

## File handles

Requires `builtin/fs` import.

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    let h = file_open("out.dat", "wb");
    file_write(h, "header\n");
    file_write(h, "body\n");
    file_seek(h, 0, "set");
    let all = file_read(h);
    file_close(h);
    print all;
}

run main;
```

| Builtin | Args | Returns |
|---------|------|---------|
| `file_open(path, mode)` | | `"file:N"` handle or empty on failure |
| `file_close(handle)` | | `"true"` / `"false"` |
| `file_read(handle)` | | rest of file as string |
| `file_read(handle, count)` | byte count | up to count bytes |
| `file_write(handle, data)` | | bytes written |
| `file_seek(handle, offset, whence?)` | whence: `"set"`, `"cur"`, `"end"` | `"true"` / `"false"` |
| `file_tell(handle)` | | current byte position |
| `file_flush(handle)` | | `"true"` / `"false"` |

**Modes:** `"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, `"ab"`, `"r+"`, `"w+"`, `"a+"`.

Handle format is `"file:N"` where N is an internal id. Empty return from `file_open` means the path is invalid or the mode is wrong.

## String buffers

For building large strings piece by piece:

| Builtin | |
|---------|--|
| `strbuf_new()` | new buffer handle |
| `strbuf_append(buf, text)` | append |
| `strbuf_to_string(buf)` | get result |
| `strbuf_len(buf)` | current length |
| `strbuf_clear(buf)` | reset to empty |
| `strbuf_reserve(buf, n)` | pre-allocate capacity |
| `strbuf_free(buf)` | release |

```elan
@erelang

public action main {
    let b = strbuf_new();
    let i = 0;
    while (i < 5) {
        strbuf_append(b, "line {i}\n");
        i = i + 1;
    }
    print strbuf_to_string(b);
    strbuf_free(b);
}

run main;
```

## Pointers

Available for bootstrap / compiler work:

| API | Role |
|-----|------|
| `ptr_new(value)` | create a pointer handle |
| `ptr_get(ptr)` | dereference |
| `ptr_set(ptr, value)` | write through pointer |
| `ptr_valid(ptr)` | `"true"` if still alive |
| `ptr_free(ptr)` | release |
| `malloc(n)` | allocate n bytes |
| `free(ptr)` | release |
| `&var` | take address of a variable |

See [examples/test.elan](../examples/test.elan) for a pointer sample.

## Casts

`reinterpret_cast<T>(x)`, `bit_cast<T>(x)`, `dynamic_cast<T>(x)` — for low-level type manipulation.

## Related

- [filesystem.md](filesystem.md)
- [strings.md](strings.md)
- [binary.md](binary.md)
