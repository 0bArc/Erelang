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

## Related

- [strings.md](strings.md)
- [automation.md](automation.md)
