# Collections

Runtime collections use **handles** (`list:N`, `dict:N`) returned by literal syntax. Use `[...]` for lists and `{...}` for dicts: these are the only supported forms.

No import needed.

## Lists

Create with a literal and iterate with `for`:

```elan
@erelang

public action main {
    nums = [10, 20, 30];

    for (n : nums) {
        print "n={n}";
    }
}

run main;
```

## Dictionaries

### Map literal syntax

```elan
@erelang

public action main {
    cfg = {"host": "localhost", "port": "8080"};

    print "host={cfg.host}";
    print "port={cfg.port}";
}

run main;
```

### Iterating dict entries

`for (key, value : dict)` iterates all entries:

```elan
@erelang

public action main {
    scores = {"alice": "95", "bob": "82", "carol": "77"};
    for (name, score : scores) {
        print "{name}: {score}";
    }
}

run main;
```

## Sized collections

Put `{N}` after the variable name to give a collection a size up front.

### Fixed-length arrays

`Array<T> name {N}` is an array of exactly `N` elements. Elements from the
initializer come first, and the rest are filled with the zero value of `T`
(`0`, `0.0`, `false` or `""`).

```elan
@strict

public action main() {
    Array<int> a {6} = [1, 2, 3];   // [1, 2, 3, 0, 0, 0]
    Array<string> names {3};        // ["", "", ""]
    a[5] = 42;                      // in-bounds writes are fine
    names[1] = "bob";
    print a.len();                  // 6

    int n = 4;
    Array<double> ds {n * 2};       // the size can be any int expression
}

run main;
```

Rules:

- The length never changes. `push`, `pop`, `insert`, `erase` and `clear` throw
  `cannot push to a fixed-size array (length 6)` and similar errors.
- `a[i] = v` with `i` outside `0..N-1` throws `list index 6 out of bounds (length 6)`.
- A literal initializer with more than `N` elements is a compile error (`TC172`).
  When `N` is only known at run time, the same check runs at run time.
- `Array<int> b {4} = other;` copies `other`, so the source list keeps its own length.

### Capacity for maps and sets

On `Map` and `Set` the size only reserves space. Entries can still be added
without limit.

```elan
Map<string, int> counts {1024};
counts["apples"] = 3;
```

### Index assignment

`list[i] = v` replaces an element and `map[key] = v` inserts or replaces an entry.

## JSON

`to_json(value)` serializes a dict or list to a JSON string.
`from_json(text)` parses a JSON object into a dict handle.

```elan
@erelang

public action main {
    d = {"x": "1", "y": "2"};
    json = to_json(d);
    print json;
}

run main;
```

## Notes

- List and dict literals are the primary collection API.
- `for (item : list)` and `for (key, value : dict)` are the primary iteration forms.
- Dict field access uses dot syntax: `cfg.host`.

## Related

- [data.md](data.md): persistent key/value stores
- [filesystem.md](filesystem.md): `list_files`
- [language.md](language.md)
