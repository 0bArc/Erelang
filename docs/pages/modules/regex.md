# Regular expressions

Wraps `std::regex` (ECMAScript syntax). All functions take `(text, pattern)` in that order.

```elan
#include <builtin/regex> as re
```

| Alias | Builtin | Args | Returns |
|-------|---------|------|---------|
| `re.match(text, pattern)` | `regex_match` | | `"true"` if the **entire** text matches the pattern |
| `re.find(text, pattern)` | `regex_find` | | first match found anywhere in text, or empty |
| `re.replace(text, pattern, repl)` | `regex_replace` | | string with all matches replaced |
| `re.expect(pattern, text)` | `regex_expect` | | `true` if the whole text matches the [expect pattern](#expect-patterns) |
| `re.infer(sample)` | `regex_infer` | | compact regex for text shaped like the sample, see [AI](ai.md) |
| `re.explain(pattern)` | `regex_explain` | | the regex an expect pattern expands to |

**Important:** `re.match` requires the **whole string** to match. Use `re.find` to search for a pattern anywhere in the text.

## Example

```elan
@erelang
#include <builtin/regex> as re

public action main {
    text = "order-2026-0042";

    if (re.match(text, "order-[0-9]+-[0-9]+") == "true") {
        print "full match";
    }

    if (re.find(text, "[0-9]+") != "") {
        print "contains digits";
    }

    first_num = re.find(text, "[0-9]+");
    print "first number={first_num}";

    cleaned = re.replace(text, "[0-9]", "X");
    print "cleaned={cleaned}";
}

run main;
```

Expected output:
```
full match
contains digits
first number=2026
cleaned=order-XXXX-XXXX
```

## Expect patterns

`re.expect(pattern, text)` matches the **whole** text against a readable pattern, so common checks
need no regex syntax. Note the argument order: pattern first, text second.

```elan
string match = "20xx20xx20";
if (re.expect(match, "2024202520")) {
    print "matches";
}
```

| Symbol | Matches |
|--------|---------|
| `x` | one digit |
| `X` | one letter (A-Z, a-z) |
| `?` | any one character |
| `*` | any run of characters, possibly empty |
| backslash + `c` | the literal character `c` |
| anything else | itself (regex special characters such as `.` are escaped for you) |

!!! warning "Lowercase `x` is a digit"
    A literal `x` in the pattern matches a digit, so `report.txt` is written `report.t\xt`.
    In an Erelang string literal the backslash is doubled: `"report.t\\xt"`.

`re.explain(pattern)` returns the regex an expect pattern expands to, which is handy for learning
or for passing to `re.match`:

```elan
print re.explain("20xx20xx20");   // 20[0-9][0-9]20[0-9][0-9]20
```

A runnable version is in `examples/std/regex_expect.elan`.

The [AI module](ai.md) can write these patterns for you: `ai.regex.generate("3 digits then a dash")`.

## Related

- [strings.md](strings.md)
- [automation.md](../advanced/automation.md)
