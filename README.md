# Erelang Reference (Capabilities, Syntax, and Methods)

This README is a practical language reference for **Erelang** in this repository.
It covers what the language can do, core syntax, and the major built-in methods available in the runtime.

## What Erelang Can Do

- Build scriptable applications with actions, entities, hooks, and globals.
- Run imperative control flow (`if`, `switch`, `for`, `while`, `for .. in`).
- Use lists, dictionaries, and binary buffers through built-in APIs.
- Work with files, paths, environment variables, and process helpers.
- Use networking helpers (`http_get`, downloads, URL encode, DNS/IP helpers).
- Use concurrency (`parallel`, thread built-ins) and monitoring built-ins.
- Build Windows UI with `win_*` built-ins.
- Import modules (local files, std modules, plugin-style imports).
- Compile scripts to standalone executables via the CLI.

## Minimal Program

```erelang
@erelang

@entry("main")
public action main {
    print "Hello Erelang"
}

run main
```

## File Structure and Directives

- `@erelang` enables Erelang dialect behavior.
- `import "path"` imports local, std, or plugin modules.
- `@entry("main")` marks an action as an entry action.
- `run main` selects the startup action.

## Comments

```erelang
// line comment
/// doc line comment
/* block comment */
/** doc block comment */
```

## Identifiers and Literals

- Integers: `42`, `-9`
- Strings: `"hello"`, interpolation `"value={x}"`
- Booleans: `true`, `false`
- Durations: `250ms`, `5s`, `2m30s`
- Units: `9m`, `15kg`, `60km/h`

## Attributes

- `@entry("name")`
- `@plugin("slug")`
- `@strict`
- `@hidden`
- `@event("actionName")`

Attributes are placed directly above declarations.

## Visibility

- `public` exposes declarations across module boundaries.
- No modifier means internal scope.

## Declarations

### Actions

```erelang
public action greet(name: str) {
    print "Hello {name}"
}
```

- Parameters: `name: type`
- Common type tags: `str`, `int`, `bool`
- Return: `return value`

### Hooks

```erelang
hook onStart { print "start" }
hook onEnd   { print "end" }
```

### Globals

```erelang
global apiBase = "https://api.example.com"
```

### Entities

```erelang
public entity Person {
  public field name: str
  public field age: int

  public action init(name: str, age: int) {
    self.name = name
    self.age = age
  }

  public action describe(): str {
    return "{name} ({age})"
  }
}

action main {
  let p = new Person("Erelang", 27)
  print p.describe()
}
```

## Statements

### Variables and Assignment

```erelang
let counter = 0
counter = counter + 1
```

### Conditionals

```erelang
if (score >= 90) {
    print "A"
} else if (score >= 80) {
    print "B"
} else {
    print "C"
}
```

### Switch

```erelang
switch state {
  case "idle"  { print "Waiting" }
  case "busy"  { print "Working" }
  default       { print "Unknown" }
}
```

### Loops

```erelang
for (let i = 0; i < 5; i = i + 1) {
    print i
}

while (i > 0) {
    i = i - 1
}

for (let item in items) {
    print item
}
```

### Parallel and Wait

```erelang
parallel {
    print "task A"
    print "task B"
}
wait all
```

### Sleep

```erelang
sleep 250ms
```

## Operators

- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Logical: `&&`, `||`, `!`
- Coalesce: `??`
- Assignment: `=`

## Built-in Methods (Major Categories)

Note: exact availability also depends on policy settings and runtime build flags.

### Core / Environment

- `now_ms()`, `now_iso()`
- `env(name)`
- `username()`, `machine_guid()`, `hwid()`
- `exec(cmd)`, `run_file(path)`, `run_bat(path)`

### Filesystem

- `read_text(path)`, `write_text(path, text)`
- `file_exists(path)`
- `list_files(path)`
- path/dir/copy/move/delete helpers from runtime filesystem built-ins

### Lists

- `list_new()`
- `list_push(list, value)`
- `list_get(list, index)`
- `list_len(list)`
- `list_clear(list)`

### Dictionaries

- `dict_new()`
- `dict_set(dict, key, value)`
- `dict_get(dict, key)`
- `dict_has(dict, key)`
- `dict_keys(dict)`, `dict_values(dict)`

### Math

- arithmetic/trig helpers (for example `add`, `sub`, numeric helpers)
- power/abs/min/max style helpers

### Crypto

- `hash_fnv1a(text)`
- `random_bytes(count)`

### Network

- `http_get(url)`
- `http_download(url, path)`
- URL encoding and DNS/IP helper functions

### Binary Buffers

- `bin_new()`
- `bin_from_hex(hex)`
- byte push/get helpers
- hex serialization helpers

### Permissions

- `perm_grant(name)`
- `perm_revoke(name)`
- `perm_has(name)`
- `perm_list()`

### Threads

- `thread_run(actionName)`
- `thread_join(handle)`
- timeout/state/query helpers
- `thread_wait_all()`

### Monitor

- file monitor create/query helpers (`monitor_*`)

### Data Store

- handle-based key/value store helpers (`data_*`)

### Windows GUI

- `win_window_create(title, w, h)`
- `win_button_create(...)`
- `win_show(window)`
- `win_loop(eventAction)`
- additional `win_*` controls/utilities

## Imports and Modules

```erelang
import "std/ui"
import "../lib/module.0bs"
import "@plugin.slug"
```

## CLI Usage

Build:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

Run:

```powershell
./build/erelang.exe examples/program.0bs
```

Compile to exe:

```powershell
./build/erelang.exe --compile examples/program.0bs --output program.exe
```

## Current Known Gaps

Based on repository docs and manifest:

- Some parsed statements (for example `wait all`, `pause`, `input`) may be partially implemented at runtime.
- Type checker coverage may lag behind newest runtime built-ins in some areas.
- FFI/GC/stdlib internals have placeholder components in current sources.

## Where to Read More

- `syntax.md`
- `erevos_full_guide.md`
- `docs/guide.md`
- `docs/tutorial.md`
- `docs/erevos.md`
- `implemented.manifest`
- `examples/`
