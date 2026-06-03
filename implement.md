# Implementation backlog

Things needed to make Erelang feel good to write.

---

## Collections — typed method syntax

Replace the old `list_get(list, i)` / `dict_has(dict, k)` handle API with typed collections and method access.

**Declaration:**
```elan
let nums = List<int>();
let names = List<string>();
let config = Dict<string, string>();
```

**Literal sugar stays as-is** — `[1, 2, 3]` infers `List<int>`, `{"k": "v"}` infers `Dict<string, string>`.

**List methods:**
```elan
nums.add(10);
nums.add(20);
print nums.len();
print nums.get(0);
nums.remove(0);
nums.clear();
print nums.contains(10);
let s = nums.join(", ");
let sub = nums.slice(1, 3);
nums.sort();
nums.reverse();
```

**Dict methods:**
```elan
config.set("host", "localhost");
print config.get("host");
print config.has("host");
config.remove("host");
let keys = config.keys();
let vals = config.values();
config.clear();
print config.size();
```

**Index access sugar:**
```elan
print nums[0];
config["host"] = "localhost";
```

**What needs implementing:**
- Generic type parameter parsing in parser (`List<T>`, `Dict<K, V>`)
- Method dispatch on typed collection handles
- Typechecker awareness of typed collection methods and signatures
- Index operator `[]` for get/set sugar
- `from_json` returning typed `Dict<string, string>` and `List<string>`

---

## Type system

Currently everything is a string at runtime. This causes the `== "true"` pattern everywhere.

- **Real bool type** — `true`/`false` as first-class values, not strings. `if (x)` should just work.
- **int and float as distinct types** — arithmetic should not go through string conversion
- **Type inference** — `let x = 10` infers `int`, `let s = "hello"` infers `string`
- **Type annotations** — `let x: int = 10;` as optional declaration
- **Null / none type** — distinguish missing value from empty string. `let x: string? = none;`

---

## Action return values

Actions are currently void — you cannot return a value.

```elan
public action add(a: int, b: int) -> int {
    return a + b;
}

let result = add(3, 4);
print result;
```

- Parser: `-> type` return type annotation
- Typechecker: validate return type matches
- Runtime: propagate return value from `return` statement through action call

---

## Strings

- `string.split(s, sep)` — split by separator, return `List<string>`
- `string.contains(s, sub)` — bool (sugar over `string.find`)
- `string.replace(s, old, new)` — plain replace, no regex
- `string.repeat(s, n)` — repeat n times
- `string.pad_left(s, width, char?)` — left-pad to width
- `string.pad_right(s, width, char?)` — right-pad
- `string.trim_prefix(s, prefix)` — remove prefix if present
- `string.trim_suffix(s, suffix)` — remove suffix if present
- `char_at(s, i)` — character at index
- `ord(c)` / `chr(n)` — char ↔ codepoint
- Multi-line string literals — triple-quote `"""..."""` or backtick

---

## Operators and expressions

- **Ternary** — `let x = cond ? a : b;`
- **Null coalescing** — `let x = maybe ?? "default";`
- **Compound assignment** — `x += 1;`, `x -= 1;`, `x *= 2;`
- **Range literals** — `0..10` or `0..=10` for use in for loops and slice
- **Pipe operator** — `text |> string.trim |> string.lower` — chains calls left to right, great for automation
- **Logical operators** — `&&`, `||`, `!` as proper bool operators (currently string-based)
- **Hex integer literals** — `0xFF` parsed as 255, not string

---

## Control flow

- **`for (i in 0..10)`** — numeric range loop without a while counter
- **`for (i, item : list)`** — indexed iteration
- **Pattern matching** — extended `switch` that matches types, ranges, or destructures:
  ```elan
  switch (x) {
      case 0       -> print "zero";
      case 1..9    -> print "single digit";
      case string  -> print "it's a string";
  }
  ```
- **`loop { ... }`** — infinite loop with explicit `break`

---

## Error handling

- `try { ... } catch (e) { ... }` — structured error handling
- `assert(cond, msg)` — halt with message if false
- Action return on failure — `let result = risky() or exit(1);`
- Runtime errors currently crash the whole script with no recovery path

---

## Closures and first-class actions

```elan
let greet = action(name: string) {
    print "hello {name}";
};

greet("world");

let nums = [1, 2, 3, 4, 5];
let evens = nums.filter(action(n) { return n % 2 == 0; });
```

- Actions as values — pass them to other actions, store in variables
- Inline lambda / anonymous action syntax
- `List.filter(fn)`, `List.map(fn)`, `List.reduce(fn, init)`

---

## Destructuring

```elan
let [a, b, c] = [10, 20, 30];
let {host, port} = {"host": "localhost", "port": "8080"};
```

- List destructuring in `let`
- Dict destructuring in `let`
- Swap without temp: `let [x, y] = [y, x];`

---

## Variadic actions

```elan
public action log(...args) {
    for (a : args) {
        print a;
    }
}

log("a", "b", "c");
```

---

## Enums

Currently enums exist but are bare names. Add associated values and methods:

```elan
enum Status {
    Ok,
    Err(string),
    Pending(int),
}

let s = Status.Err("timeout");
switch (s) {
    case Status.Ok       -> print "done";
    case Status.Err(msg) -> print "error: {msg}";
}
```

---

## Filesystem

- `fs.glob(dir, pattern)` — glob match, return `List<string>`
- `fs.walk(dir)` — recursive walk, return `List<string>`
- `fs.is_dir(path)` — bool
- `fs.copy_dir(src, dst)` — recursive copy
- `fs.remove_dir(path)` — recursive delete
- `fs.temp_file()` — create a temp file, return path
- `fs.temp_dir()` — create a temp dir, return path

---

## Network

- `net.headers(url)` — return response headers as `Dict<string, string>`
- `net.post_json(url, dict)` — serialize dict → JSON and POST
- `net.get_json(url)` — GET, parse response as dict
- `net.put(url, body)`, `net.delete(url)` — HTTP verbs
- HTTP timeout and retry control

---

## Process / shell

- `exec_capture(cmd)` — run command, return stdout as string (no `builtin/system` needed)
- `env_set(key, value)` — set env var for child processes
- `env_all()` — all env vars as `Dict<string, string>`
- `which(name)` — find executable path, or empty

---

## Math

- `math.floor(x)`, `math.ceil(x)`, `math.round(x)` — float rounding
- `math.log(x)`, `math.log2(x)`, `math.log10(x)`
- `math.pi`, `math.e` — constants as named values
- Float arithmetic — operators should work correctly on float values without going through int conversion

---

## Builtins (small gaps)

- `sleep(ms)` as a function call, not only `sleep Nms` statement syntax
- `now_unix()` — Unix timestamp as integer
- `format(template, ...args)` — printf-style, e.g. `format("hello %s, you are %d", name, age)`
- `parse_int(s, base?)` — parse with explicit base (enables `parse_int("FF", 16)` → 255)
- `min(a, b)` / `max(a, b)` as core builtins without importing math

---

## Module system

- **Named imports without alias** — `use builtin/fs;` then call `fs.read(...)` automatically
- **Re-export** — a `.elan` module can re-export from another module
- **Module-level constants** — `const PI = 3.14159;` at file scope
- **Circular import detection** — currently silent, should be a TC error
- **Version pinning in project manifest** — `#include <mylib@1.2>` or via project file

---

## Formatting / output

- `print_err(msg)` — alias for `stderr_print` that's less verbose
- `printf(fmt, ...args)` — formatted print without newline
- Color output as core (not buried in obs_main) — `print color.red("error: {msg}")`
- `print_table(list_of_dicts)` — tabular output for CLI tools

---

## Testing

Built-in test block support:

```elan
test "addition works" {
    assert math.add(2, 3) == 5;
}

test "split returns list" {
    let parts = string.split("a,b,c", ",");
    assert parts.len() == 3;
    assert parts.get(0) == "a";
}
```

- `erelang test file.elan` — run all test blocks, report pass/fail
- `assert(cond)` and `assert_eq(a, b)` as test helpers

---

## Tooling

- REPL — `erelang repl`, interactive session
- `--watch` — re-run script on file change
- Stack trace on runtime error with line numbers
- `erelang fmt` — auto-format .elan files
- LSP (language server) — go-to-definition, hover types, completions in any editor
- `erelang check file.elan` — typecheck only, no run
