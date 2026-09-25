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

## Generics

Parametric types and actions use `<T>` / `<A, B>` with optional constraints `T: Trait` or `T: A & B`. Generic bodies are verified against trait methods only (opaque checking). See [syntax.md](syntax.md#generics) and `examples/generics.elan`.

## Option / Result and match

`Option<T>` and `Result<T, E>` are ordinary generic enums (not builtins):

```elan
enum Option<T> {
    Some(T),
    None
}

enum Result<T, E> {
    Ok(T),
    Error(E)
}

Option<int> o = Option<int>.Some(42);
match (o) {
    case Some(v): { print v; }
    case None: { print "empty"; }
}
```

- Construction: `Type.Variant(payloads...)` / `Type.Variant` for zero-payload.
- Match binds payload names only inside the case body; types come from specialized enum args.
- Nested patterns: `case Ok(Some(x)):` when the payload is itself an enum.
- Multi-payload: `case Values(a, b):` with arity checked at typecheck.
- Guards: `case Some(x) if x > 0:`.
- Or-patterns: `case Red | Green:`.
- Exhaustiveness: enums must cover every variant or include `case _:` (`TC161`).
- Enum `let` destructuring: `let Some(x) = o;`, `let Ok(v) = r;`, `let None() = empty;` (mismatch throws at runtime).

## Namespaces

```elan
namespace Math {
    public action add(int a, int b): int { return a + b; }
}
print Math::add(1, 2);
```

Names inside a namespace are mangled as `Outer::Inner::name`. Call from outside with `::`. Nested namespaces allowed. This is name scoping only — not a package registry. Multi-file programs use `#include` / `import` path resolution (see [imports.md](imports.md)). Remote package registry is out of scope.

## Ranges, unit, never

- Inclusive range `a .. b` and half-open `a ..< b` produce `array<int>` (usable in `for (i : 1 .. 3)`).
- `unit` is an alias for `void`.
- `never` is the type of `fail(msg)` and of actions that always throw. `fail` throws `std::runtime_error`.
- See `examples/range_unit_never.elan`, `examples/match_guards.elan`.

## Memory (`*T`, own, shared, weak, buffer)

Native pointers and ownership — see [memory.md](memory.md).

```elan
*int p = alloc<int>();
*p = 42;
free(p);

heap<int> o = heap<int>(1);
shared<int> s = shared<int>(2);
weak<int> w = weak(s);
buffer<u8> buf = buffer<u8>(64);
buf[0] = 9;
```

No borrow checker. Prefer `heap` / `buffer`. Primitives: `alloc`, `free`, `sizeof`, `alignof`.

## Pipe, future, mutex

```elan
#include <std/pipe>

string p = pipe.new(2);
p.send("x");
string v = p.receive();
p.close();
any ok = fut.cancel();
print fut.done();
string m = mutex_new();
mutex_lock(m); mutex_unlock(m);
```

`fut.cancel()` while a task is blocked in `p.receive()` / `p.send()` unblocks that wait. See `examples/channel_sync.elan`.

## Declaration destructuring

```elan
let { name, age } = user;
let [first, second] = values;
let { name: renamed } = user;
let (a, b) = (1, "hello");
let pair = (10, 20);
```

- Struct/entity field patterns, array element patterns, and tuple patterns share the match `Pattern` AST.
- Extra array elements ignored; too few elements fail at runtime.
- Tuple type is `tuple<T1, T2, ...>` (heterogeneous); arrays stay homogeneous.
- See `examples/destructure.elan`, `examples/tuple.elan`, `examples/enum_let.elan`.

## Result / Option helpers and `?`

```elan
print result.unwrap();
print option.unwrap_or(0);
match (result.ok()) { case Some(v): { ... } case None: { ... } }

public action foo(): Result<int, string> {
    int x = get_value()?;
    return Result<int, string>.Ok(x);
}
```

- `.unwrap()` / `.ok()` / `.unwrap_or(default)` operate on enum-encoded Option/Result (not legacy string builtins).
- Postfix `?` propagates `Error`/`None` as an early return; return type must match (TC158/TC159).
- See `examples/result_helpers.elan`, `examples/try_op.elan`.

## Async / await

`async action` runs on a fixed worker pool (`max(2, hardware_concurrency)`). Calling an async action from another async action yields `future<T>` (handle prefix `future:`). `await` waits for the result and rethrows errors stored on the future. Calling an async action from a normal action runs it to completion and returns `T` directly.

```elan
public async action slow(): int {
    return 7;
}
public async action main(): int {
    int x = await slow();
    print x;
    return x;
}
```

`await` is only legal inside `async` actions (`TC160`). `await` of `future<Result<T,E>>` / `future<Option<T>>` yields the enum type. Postfix `?` after await unwraps that Result/Option: `int x = await get()?;`. While awaiting on a pool worker, the worker helps run other queued tasks so the pool does not deadlock. See `examples/async_await.elan`.

## Function types

Higher-order parameters use `action(T, U) -> R` (or `(T, U) -> R`):

```elan
public action map_opt<T, U>(value: Option<T>, transform: action(T) -> U): Option<U> {
    match (value) {
        case Some(v): { return Option<U>.Some(transform(v)); }
        case None: { return Option<U>.None; }
    }
}
```

Pass a `lambda(...)` (or any `func:` handle). See `tests/types/function_types.elan`.

Generic *function values* work: store a generic action in a variable (`any f = id;` or an `action(...)` type) and call it; type args are inferred at the call site when needed. See `tests/types/generic_function_value.elan`.

## Associated types

Traits may declare associated types; implementing types bind them with a type alias `Type::Name = ...` (same `::` type grammar as namespaces). Call sites may name `T::Item` when `T` is constrained.

```elan
trait Container {
    type Item;
    get(i: int): Item;
}

struct Box {
    int value;
    action get(i: int): int { return self.value; }
}

type Box::Item = int;

public action first<T: Container>(t: T): T::Item {
    return t.get(0);
}
```

Missing bindings diagnose `TC164`; return mismatches against the associated type diagnose `TC165`.

## Closure capture

A lambda that assigns to a captured local updates the outer variable (shared cell) when that name is a local in the enclosing action. See `tests/closures/capture_mutate.elan`.

## Operator overloading

Primitives keep builtin ops (`int + int`, `string + string`). For user types, operators lower to trait-style methods when present:

| Op | Trait | Method |
|----|-------|--------|
| `+` | `Add<T>` | `add(other: T): T` |
| `-` | `Sub<T>` | `sub(other: T): T` |
| `*` | `Mul<T>` | `mul(other: T): T` |
| `/` | `Div<T>` | `div(other: T): T` |
| `==` `!=` | `Eq<T>` | `eq(other: T): bool` |
| `[]` | `Index<T>` | `get(index: int): T` |

Opaque type parameters need the matching trait in the constraint list (`T: Add<T>`) or `TC140` fires. Concrete structs/entities only need the method. See `examples/operator_add.elan`.

## Variables

```elan
x = 2;
int y = 10;
const PI = 3;
```

Values are `int`, `float`, `bool`, `string`, `null`, or handles. Numeric operators use the numeric types.

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
- [crypto.md](crypto.md) — `builtin/crypto` (SHA-256, AES-256-GCM, …)
