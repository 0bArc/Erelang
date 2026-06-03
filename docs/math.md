# Math

## Builtin module

```elan
#include <builtin/math> as math
```

| Call | Notes |
|------|-------|
| `math.add(a, b)` | integer addition |
| `math.sub(a, b)` | subtraction |
| `math.mul(a, b)` | multiplication |
| `math.div(a, b)` | integer division (div by zero returns 0) |
| `math.mod(a, b)` | remainder |
| `math.min(a, b)` | minimum |
| `math.max(a, b)` | maximum |
| `math.abs(a)` | absolute value |
| `math.pow(base, exp)` | integer power |
| `math.sqrt(a)` | integer square root |
| `math.sin(a)` | sine |
| `math.cos(a)` | cosine |
| `math.tan(a)` | tangent |

After importing, bare names (`add(1, 2)`) also work — builtins are registered for the whole program once imported.

## Example

```elan
@erelang
#include <builtin/math> as math

public action main {
    print math.add(10, 5);
    print math.pow(2, 8);
    print math.min(7, 3);
    print math.abs(-42);
}

run main;
```

## Local `.elan` math module

`examples/modules/math.elan` provides user-defined actions (`sum`, `sub`, `mul`, `div`, `mod`, `pow`, `abs`, `min`, `max`, `is_even`, `clamp`):

```elan
@erelang
#include <examples/modules/math.elan>

public action main {
    print sum(10, 5);
    print is_even(4);
    print clamp(150, 0, 100);
}

run main;
```

Including a `.elan` file merges its `public action`s. Unused exports warn with `TC130`.

## Which to use

| Use case | Choice |
|----------|--------|
| Fast integer ops | `builtin/math` |
| Custom logic, no C++ build | local `.elan` module |

## Related

- [imports.md](imports.md)
- [language.md](language.md)
