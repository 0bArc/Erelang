# Memory model

Native ownership and raw memory for self-host. No borrow checker. Plain structs still copy on assign. Use `heap` / `shared` / `weak` / `buffer` for explicit ownership.

## Types

| Type | Meaning |
|------|---------|
| `*T` | Raw pointer (also `T*`) |
| `heap<T>` | Unique owner; move on assign; drop frees |
| `shared<T>` | Refcounted; copy shares; last drop frees |
| `weak<T>` | Non-owning; `w.get()` → `Option<T>` |
| `buffer<T>` | Owned contiguous buffer |

`u8` is an integer alias (byte-sized for `sizeof` / `alignof`).

## Raw alloc (language primitives)

```elan
*int p = alloc<int>();
*int q = alloc<int>(10);
*p = 42;
free(p);

print sizeof<int>();
print alignof<u8>();

int x = 1;
*int r = &x;
*r = 2;
*(q + 1) = 9;
```

Prefer `heap` / `buffer` for most code. Do not use removed globals: `ptr_new`, `ptr_get`, `malloc`, `make_unique`.

## heap\<T\>

```elan
heap<int> a = heap<int>(42);
print *a;
heap<int> b = a;   // move: a empty; *a is a runtime error
```

Moved-from drop is a no-op.

## shared\<T\> / weak\<T\>

```elan
shared<int> a = shared<int>(10);
shared<int> b = a;
*b = 99;
weak<int> w = weak(a);
match (w.get()) {
    case Some(v): { print v; }
    case None: { print "expired"; }
}
```

## buffer\<T\>

```elan
buffer<u8> data = buffer<u8>(1024);
data[0] = 42;
data.resize(2048);
data.push(value);
data.pop();
data.clear();
data.reserve(n);
print data.len();
print data.cap();
*u8 p = data.ptr();
```

## Runtime notes

- Handles use prefixes `ptr:`, `heap:`, `shared:`, `weak:`, `buffer:`.
- Scope exit drops locals that own (`heap` / `shared` / `buffer` / `weak` / owned `file:`).
- No GC. No `std.memory` module. No `own<T>` (renamed to `heap<T>`).
- All language heap objects go through this API: `alloc`/`free`/`realloc`, `heap`/`shared`/`weak`/`buffer`. Lists/dicts use the same handle registry and clear on run reset.
- Debug counters (no GC): `mem_stats()` → `allocs=… frees=… live=… double_frees=… use_after_free=…`; `mem_alive()` → live count. Double-free and use-after-free throw; counters still bump.
