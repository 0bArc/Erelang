# Core builtins

Available in every program without importing a module.

## Time

| Builtin | Args | Returns |
|---------|------|---------|
| `now_ms()` | | milliseconds since Unix epoch |
| `now_iso()` | | ISO-8601 timestamp string e.g. `"2026-06-03T14:22:00"` |

```elan
@erelang

public action main {
    print now_iso();
    print now_ms();
}

run main;
```

## Random

| Builtin | Args | Returns |
|---------|------|---------|
| `rand_int(min, max)` | inclusive range | random int as string |
| `uuid()` | | UUID v4 string |

```elan
@erelang

public action main {
    print rand_int(1, 100);
    print uuid();
}

run main;
```

## Environment

| Builtin | Args | Returns |
|---------|------|---------|
| `env(name)` | variable name | value string, or empty if not set |

```elan
@erelang

public action main {
    home = env("USERPROFILE");
    print "home={home}";
}

run main;
```

## CLI arguments

| Builtin | Args | Returns |
|---------|------|---------|
| `args_count()` | | number of args after script name |
| `args_get(i)` | 0-based index | argument string |

```elan
@erelang

public action main {
    n = args_count();
    print "args={n}";
    if (n > 0) {
        print "first={args_get(0)}";
    }
}

run main;
```

## Console I/O

| Builtin | Notes |
|---------|-------|
| `input(prompt)` | prints prompt, reads a line from stdin |
| `read_line()` | reads a line from stdin, no prompt |
| `stderr_print(msg)` | writes to stderr |

`print` is a statement, not a builtin call:

```elan
print "hello";
print 42;
print "value={x}";
```

## Process

| Builtin | Args | Returns |
|---------|------|---------|
| `exec(cmd)` | command line string | exit code as string |
| `spawn(cmd)` | command line string | process id (detached) |
| `exit(code)` | int | terminates immediately |

## Type conversions

| Builtin | Returns |
|---------|---------|
| `toint(x)` | integer string |
| `tostr(x)` | string |
| `tofloat(x)` | float string |
| `tobool(x)` | `"true"` / `"false"` |

## Type checks

| Builtin | Returns |
|---------|---------|
| `is_int(x)` | `"true"` / `"false"` |
| `is_float(x)` | `"true"` / `"false"` |

## Strings (no import)

| Builtin | Args | Returns |
|---------|------|---------|
| `string.len(s)` | | length |
| `string.lower(s)` | | lowercase |
| `string.upper(s)` | | uppercase |
| `string.strip(s)` | | trimmed |
| `string.lstrip(s)` | | left-trimmed |
| `string.rstrip(s)` | | right-trimmed |
| `string.find(s, sub)` | | index or `-1` |
| `string.substr(s, start, len?)` | | substring |
| `string.starts_with(s, prefix)` | | bool string |
| `string.ends_with(s, suffix)` | | bool string |

## JSON

| Builtin | Returns |
|---------|---------|
| `to_json(value)` | JSON string |
| `from_json(text)` | dict handle |

## Collections (no import)

Lists: `list_new`, `list_get`, `list_len`, `list_push`, `list_join`, `list_clear`, `list_remove_at`

Dicts: `dict_new`, `dict_set`, `dict_get`, `dict_has`, `dict_keys`, `dict_values`, `dict_size`, `dict_remove`, `dict_clear`, `dict_merge`, `dict_clone`

Full docs: [collections.md](collections.md)

## Module metadata

| Builtin | |
|---------|--|
| `language_name()` | `"erelang / Erelang"` |
| `language_version()` | version string |
| `plugin_core(slug, key)` | plugin core property |

## Not in core (import required)

| Need | Module |
|------|--------|
| Files | `builtin/fs`: [filesystem.md](filesystem.md) |
| HTTP | `builtin/network`: [network.md](network.md) |
| Math | `builtin/math`: [math.md](math.md) |
| Shell capture | `builtin/system`: [process.md](process.md) |
| Regex | `builtin/regex`: [regex.md](regex.md) |
| Crypto | `builtin/crypto`: [crypto.md](crypto.md) |
| Binary | `builtin/binary`: [binary.md](binary.md) |

## Related

- [imports.md](imports.md)
- [language.md](language.md)
