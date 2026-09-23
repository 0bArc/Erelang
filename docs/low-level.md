# Low-level APIs

Advanced scripting: streaming file handles, string builders, pointers and memory. Most automation should use the high-level helpers in [filesystem.md](filesystem.md) instead.

## File handles

Requires `builtin/fs` import. Prefer `fs.open` and handle methods:

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    string h = fs.open("out.dat", "wb");
    h.write("header\n");
    h.write("body\n");
    h.seek(0, "set");
    string all = h.read();
    h.close();
    print all;
}

run main;
```

| API | Args | Returns |
|-----|------|---------|
| `fs.open(path, mode)` | | `"file:N"` handle or empty on failure |
| `h.close()` | | `"true"` / `"false"` |
| `h.read()` | | rest of file as string |
| `h.read(count)` | byte count | up to count bytes |
| `h.write(data)` | | bytes written |
| `h.seek(offset, whence?)` | whence: `"set"`, `"cur"`, `"end"` | `"true"` / `"false"` |
| `h.tell()` | | current byte position |
| `h.flush()` | | `"true"` / `"false"` |

**Modes:** `"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, `"ab"`, `"r+"`, `"w+"`, `"a+"`.

Handle format is `"file:N"` where N is an internal id. Empty return from `fs.open` means the path is invalid or the mode is wrong.

Streaming handles — use `fs.open` and handle methods. Do not call bare `file_*` / `fopen` globals (TC rejects them).

String building: use `+` / string methods (`text.lower()`, `strip`, …), not `strbuf_*` globals.

Pointers: use `*T`, `&x`, `heap<T>`, `shared<T>`, `buffer<T>` — not `ptr_new` / `ptr_get` / `make_unique`.

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
    b = strbuf_new();
    i = 0;
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

`reinterpret_cast<T>(x)`, `bit_cast<T>(x)`, `dynamic_cast<T>(x)`: for low-level type manipulation.

## Related

- [filesystem.md](filesystem.md)
- [strings.md](strings.md)
- [binary.md](binary.md)
