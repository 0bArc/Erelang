# Collections

Runtime collections use **handles** (`list:N`, `dict:N`) returned by literal syntax. Use `[...]` for lists and `{...}` for dicts — these are the only supported forms.

No import needed.

## Lists

Create with a literal and iterate with `for`:

```elan
@erelang

public action main {
    let nums = [10, 20, 30];

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
    let cfg = {"host": "localhost", "port": "8080"};

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
    let scores = {"alice": "95", "bob": "82", "carol": "77"};
    for (name, score : scores) {
        print "{name}: {score}";
    }
}

run main;
```

## JSON

`to_json(value)` serializes a dict or list to a JSON string.
`from_json(text)` parses a JSON object into a dict handle.

```elan
@erelang

public action main {
    let d = {"x": "1", "y": "2"};
    let json = to_json(d);
    print json;
}

run main;
```

## Notes

- List and dict literals are the primary collection API.
- `for (item : list)` and `for (key, value : dict)` are the primary iteration forms.
- Dict field access uses dot syntax: `cfg.host`.

## Related

- [data.md](data.md) — persistent key/value stores
- [filesystem.md](filesystem.md) — `list_files`
- [language.md](language.md)
