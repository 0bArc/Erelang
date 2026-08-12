# Language reference

Erelang programs are collections of **actions** (functions), optional **entities** (objects), **hooks**, and **globals**, executed by the interpreter.

## Actions

```elan
public action greet(name: string) {
    print "Hello {name}";
}

public int add(a: int, b: int) {
    return a + b;
}

public action main {
    greet("World");
    print add(2, 3);
}

run main;
```

Declare functions either way:

| Style | Example |
|-------|---------|
| Classic | `public action clamp(...): int { ... }` or `-> int` |
| Type-first | `public int clamp(int val, int min_val, int max_val) { ... }` |

- Parameters and return types are optional but recommended.
- `void` return: omit type, use `void`, or use `public void main()` for clarity.
- `public` / `private` on actions, entity members, and globals; enforced when `@strict` is enabled.
- Entry point: `run <actionName>` at file bottom.

## Variables

```elan
x = 2;
int y = 10;
const PI = 3;
```

Runtime values are string-backed; numeric operators parse operands as numbers when needed.

## Strings

Double-quoted literals; `{name}` interpolation inside strings:

```elan
who = "Erelang";
print "Hi {who}";
```

## Control flow

Conditions use **bool** values (`true` / `false` keywords).

```elan
if (!fs.exists(path)) {
    print "missing";
}

if (ready == false) {
    print "not ready";
}
```

```elan
if (x < 0) {
    print "negative";
} else if (x == 0) {
    print "zero";
} else {
    print "positive";
}
```

Builtin calls such as `fs.exists(path)` return bool; prefer `if (!fs.exists(p))` over `if (fs.exists(p) == "false")`.

```elan
while (n > 0) {
    n = n - 1;
}

for (i : items) {
    print "{i}";
}

switch kind {
    case "a" { print "A"; }
    default { print "?"; }
}
```

`else if` chains work like C-style languages.

## Statements

| Statement | Example |
|-----------|---------|
| Print | `print "msg";`, `print "{name}";`, or `print expr;` |
| Sleep | `sleep 250ms;` |
| Return | `return 0;` |
| Input | `input "prompt";` |
| Parallel | `parallel { ... }` then `wait all;` |

## Entities

```elan
entity Counter {
    public field value: int;

    public action init(v: int) {
        self.value = v;
    }

    public action bump(step: int): int {
        self.value = self.value + step;
        return self.value;
    }
}

public action main {
    Counter c = new Counter();
    c.init(10);
    print c.bump(5);
}

run main;
```

## Collections (surface syntax)

```elan
Array<int> nums = [1, 2, 3];
Map<string, any> m = {"k": 9};
```

See [collections.md](collections.md) for runtime `list_*` / `dict_*` APIs.

## Structs

Structs support fields and methods; `self.field` updates the instance.

## Hooks

```elan
hook onStart { print "start"; }
hook onEnd { print "end"; }
```

Lifecycle hooks run around the main `run` target.

## Null

`null`, `nil`, and `nullptr` are equivalent at runtime.

## Attributes

`@erelang`, `@strict`, `@entry("main")`, `@event("onClick")` attach metadata for tools and runtime.

## Related docs

- [imports.md](imports.md)
- [diagnostics.md](diagnostics.md)
- [core-builtins.md](core-builtins.md)
