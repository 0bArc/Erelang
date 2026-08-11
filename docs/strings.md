# Strings

## Literals and interpolation

```elan
@erelang

public action main {
    name = "World";
    print "Hello {name}";

    x = 42;
    print "x is {x}";
}

run main;
```

Interpolation `{expr}` evaluates the expression and embeds the result. Nested strings are not supported inside `{}`.

## Core builtins (no import)

| Builtin | Args | Returns |
|---------|------|---------|
| `string.len(s)` | | character count |
| `string.lower(s)` | | lowercase copy |
| `string.upper(s)` | | uppercase copy |
| `string.strip(s)` | | whitespace-trimmed |
| `string.lstrip(s)` | | left-trimmed |
| `string.rstrip(s)` | | right-trimmed |
| `string.find(s, sub)` | | first index, or `-1` |
| `string.substr(s, start, len?)` | | substring |
| `string.starts_with(s, prefix)` | | `"true"` / `"false"` |
| `string.ends_with(s, suffix)` | | `"true"` / `"false"` |

```elan
@erelang

public action main {
    s = "  Hello, World!  ";
    print string.strip(s);
    print string.lower(s);
    print string.upper(s);
    print string.len(s);
    print string.find(s, "World");
    print string.substr(s, 2, 5);
}

run main;
```

## Type conversions

| Builtin | Returns |
|---------|---------|
| `tostr(x)` | string |
| `toint(x)` | integer string |
| `tofloat(x)` | float string |
| `tobool(x)` | `"true"` / `"false"` |

```elan
@erelang

public action main {
    n = toint("42") + 8;
    print n;
    print tostr(n);
}

run main;
```

## Concatenation

Use `+` to join strings:

```elan
@erelang

public action main {
    first = "Hello";
    second = "World";
    joined = first + ", " + second + "!";
    print joined;
}

run main;
```

## Character helpers (no import)

| Builtin | Returns |
|---------|---------|
| `char_is_digit(s)` | bool string: first char is digit |
| `char_is_alpha(s)` | bool string: first char is letter |
| `char_is_space(s)` | bool string: first char is whitespace |

## String buffers (advanced)

For building large strings incrementally without repeated concatenation:

| Builtin | |
|---------|--|
| `strbuf_new()` | new buffer handle |
| `strbuf_append(buf, text)` | append text |
| `strbuf_to_string(buf)` | get final string |
| `strbuf_len(buf)` | current length |
| `strbuf_clear(buf)` | reset |
| `strbuf_free(buf)` | release |

```elan
@erelang

public action main {
    b = strbuf_new();
    strbuf_append(b, "line one\n");
    strbuf_append(b, "line two\n");
    result = strbuf_to_string(b);
    strbuf_free(b);
    print result;
}

run main;
```

## JSON

`to_json(value)` serializes a value to JSON. `from_json(text)` parses a JSON object into a dict handle.

## Regex

Pattern match / replace: [regex.md](regex.md)

## Related

- [language.md](language.md)
- [core-builtins.md](core-builtins.md)
