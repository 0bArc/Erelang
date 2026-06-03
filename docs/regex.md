# Regular expressions

Wraps `std::regex` (ECMAScript syntax). All three functions take `(text, pattern)` in that order.

```elan
#include <builtin/regex> as re
```

| Alias | Builtin | Args | Returns |
|-------|---------|------|---------|
| `re.match(text, pattern)` | `regex_match` | | `"true"` if pattern found anywhere in text |
| `re.find(text, pattern)` | `regex_find` | | first match string, or empty |
| `re.replace(text, pattern, repl)` | `regex_replace` | | string with all matches replaced |

## Example

```elan
@erelang
#include <builtin/regex> as re

public action main {
    let text = "order-2026-0042";

    if (re.match(text, "[0-9]+") == "true") {
        print "contains digits";
    }

    let first_num = re.find(text, "[0-9]+");
    print "first number={first_num}";

    let cleaned = re.replace(text, "[0-9]", "X");
    print "cleaned={cleaned}";
}

run main;
```

Expected output:
```
contains digits
first number=2026
cleaned=order-XXXX-XXXX
```

## Related

- [strings.md](strings.md)
- [automation.md](automation.md)
