# erelang Language Guide

This guide teaches you how to write programs in erelang, run them with `erelang`, and use the built-in Windows GUI helpers. It is written for humans and aims to be clear and straightforward.

## Getting Started

- File extension: `.0bs`
- Blocks: use curly braces `{ ... }`
- Entry point: `run <actionName>`
- Print to console: `print "Hello"`
- Sleep: `sleep 250ms`

A minimal file:

```
action main {
  print "Hello from erelang"
}

run main
```

Run it from the repo root:

```
erelang .\examples\hello.0bs
```

To compile an `.exe`:

```
erelang --compile .\examples\hello.0bs --output .\build\hello.exe
```

## Language Basics

### Actions

Actions are like functions. Define them with parameters and call them by name.

```
action greet(name: str) {
  print "Hello {name}"
}

action main {
  greet("World")
}

run main
```

### Variables and Types

Use `let` to declare variables. All values are strings at runtime. Numbers are parsed on demand for math and comparison.

```
action main {
  let x = 2
  let y = 3
  print x + y           // 5
  print "a" + "b"       // ab
  print x * 10          // 20
  print x == 2          // true
}
```

### Strings and Interpolation

Wrap text in quotes. Insert variable values with `{name}` inside a string.

```
action main {
  let who = "Erelang"
  print "Hello {who}"
}
```

### Null Values

Use `null`, `nil`, or `nullptr` when you need an empty value. All three spellings compile to the same runtime representation, so pick the style that fits your project or matches a plugin's language profile.

```
action main {
  let maybeName = nullptr
  if (maybeName == null) {
    print "no name provided"
  }
}
```

### If and Switch

```
action main {
  let kind = "b"
  if (kind == "a") {
    print "A path"
  } else if (kind == "b") {
    print "B path"
  } else {
    print "other"
  }

  switch kind {
    case "a" { print "A" }
    case "b" { print "B" }
    default { print "other" }
  }
}
```

Chained `else if` blocks work exactly like other C-style languages: the parser desugars `else if` into a nested `if`, so you can write as many branches as necessary without extra braces.

### Parallel and Wait

Run statements concurrently inside a `parallel` block, then `wait all`.

```
action main {
  parallel {
    print "A"
    print "B"
  }
  wait all
}
```

### Hooks and Events

Hooks run automatically at start and end.

```
hook onStart { print "=== Start ===" }
hook onEnd   { print "=== End ===" }
```

You can also trigger a hook manually using `fire` from code you write in C++ later. In the current runtime, direct `fire` exists for predefined hooks.

## Entities (Simple Objects)

Entities group fields and methods.

```
entity Person {
  name: str
  age: int

  action init(name: str, age: int) {
    self.name = name
    self.age = age
  }

  action greet() {
    print "Hello {name}, age {age}!"
  }
}

action main {
  let p = new Person("Erelang", 999)
  p.greet()
}

run main
```

- `init` runs when you create a new entity instance with `new`.
- Access fields through `self` in methods and directly as variables in scripts when bound by the runtime.

## Struct Methods + HashMap (Self-Hosting Path)

Erelang now supports member methods inside `struct` declarations and HashMap-compatible dictionary aliases for compiler/runtime authoring workflows.

### Struct Member Methods

```
struct Counter {
  value: int

  public action bump(step: int) {
    self.value = value + step
  }

  public action read(): int {
    return value
  }
}

action main {
  Counter c = null
  c.bump(2)
  print c.read()
}
```

- `self.field` writes are supported for struct methods.
- `field` shorthand is also available inside methods and syncs back to the instance.

### HashMap Aliases

HashMap helpers map directly to dictionary runtime internals:

- `hashmap_new`, `hashmap_set`, `hashmap_put`
- `hashmap_get`, `hashmap_has`, `hashmap_contains`
- `hashmap_get_or`, `hashmap_get_or_default`
- `hashmap_remove`, `hashmap_clear`, `hashmap_size`
- `hashmap_keys`, `hashmap_values`, `hashmap_merge`

Method-style aliases are also recognized on dictionary handles:

- `put -> set`
- `contains / containsKey -> has`
- `getOrDefault -> getOr`

### Low-Level Runtime APIs

For bootstrap/compiler work and systems-style scripting, these are available:

- Memory/pointers: `malloc`, `free`, `ptr_new`, `ptr_get`, `ptr_set`, `ptr_free`, `ptr_valid`, `&var`, `*ptr`, `*ptr = value`
- Casts: `static_cast`, `dynamic_cast`, `reinterpret_cast`, `bit_cast` and `bitcast`
- File handles: `file_open`, `file_read`, `file_write`, `file_seek`, `file_tell`, `file_flush`, `file_close`
- C-style file aliases: `fopen`, `fread`, `fwrite`, `fseek`, `ftell`, `fflush`, `fclose`
- String buffers: `strbuf_new`, `strbuf_append`, `strbuf_clear`, `strbuf_len`, `strbuf_to_string`, `strbuf_reserve`, `strbuf_free`
- String-buffer aliases: `string_buffer_*`

### Pointer + malloc/free Usage

```
public action main {
  int score = 7
  let score_ptr = &score
  print *score_ptr
  *score_ptr = 88

  let raw = malloc(64)
  ptr_set(raw, "payload")
  print ptr_get(raw)
  free(raw)
}
```

### Casts: Which One and Why

- `static_cast<T>(x)`: preferred for intended conversions where types are compatible.
- `dynamic_cast<T>(x)`: checked cast semantics for object/entity-style conversions.
- `reinterpret_cast<T>(x)`: reinterpret identity/representation (pointer-style low-level conversion).
- `bit_cast<T>(x)` / `bitcast<T>(x)`: reinterpret raw bits without arithmetic conversion.

### Self-Hosted `rand` Module

The first self-hosting utility module is now in `examples/std/rand.elan`:

```
@erelang

private action next_raw(): int {
  let t = now_ms()
  return ((t * 1103515245) + 12345) % 2147483647
}

public action int(): int {
  return next_raw() - 1073741824
}

public action gen(min: int, max: int): int {
  int lo = min
  int hi = max
  if (hi < lo) {
    let tmp = lo
    lo = hi
    hi = tmp
  }
  let span = (hi - lo) + 1
  if (span <= 0) {
    return lo
  }
  return lo + (next_raw() % span)
}
```

Use it from scripts:

```
@erelang
import "std/rand.elan" as rand

public action main {
  print rand.int()
  print rand.gen(1, 20000)
}

run main
```

## Built-ins

General built-ins available on Windows:

- `now_ms()` returns the current time in milliseconds
- `env(name)` reads an environment variable
- `username()` returns the current user name
- `computer_name()` returns the computer name
- `machine_guid()` reads the Windows machine GUID
- `volume_serial()` returns the system drive volume serial
- `hwid()` returns `machine_guid:volume_serial`
- `exec(command)` launches any Win32 process and returns its exit code; pair with `cmd /c start ...` to open browsers or applications.
- `run_file(path)` and `run_bat(path)` delegate to the shell to open files or run batch scripts with their registered handlers.

Example: open the project documentation in the default browser.

```
action openDocs {
  exec("cmd /c start https://erelang.dev/docs")
}
```

All of these helpers live in `runtime.cpp` under the core built-ins table.

### Network Utilities

Networking scripts can make HTTP requests with helpers such as `http_get(url)` and `http_download(url, outPath)` from `builtins/network.cpp`. When you're debugging connectivity problems, enable structured logging with:

- `network.debug.enable([logPath])` — turns on logging and optionally overrides the log file.
- `network.debug.disable()` — stops logging while keeping the last result in memory.
- `network.debug.last()` — prints the most recent call, arguments, and result preview.

To flush or renew your IP configuration without dropping into a shell, call the new helpers (Windows only):

- `network.ip.flush()` — runs `ipconfig /flushdns` and returns a multi-line report including success, the exit code, and the command output.
- `network.ip.release([adapter])` — runs `ipconfig /release` for all adapters or the one you pass in.
- `network.ip.renew([adapter])` — runs `ipconfig /renew` for all adapters or the one you pass in.
- `network.ip.registerdns()` — runs `ipconfig /registerdns` to refresh DNS registration.

## Self-Hosting Capability Checklist (Compiler + Windows EXE)

To self host, a language must be capable of implementing an entire compiler toolchain inside itself. The requirements fall into several layers: language semantics, runtime capability, compiler infrastructure, operating system interaction, and executable generation. The list below enumerates the practical capabilities typically required for a language to compile its own compiler and produce a Windows executable such as `erelang.exe`.

1. Core syntax and semantics

Basic language building blocks.

- Variables
- Immutable variables / constants
- Variable initialization
- Assignment
- Expressions
- Operator precedence
- Arithmetic operators (`+ - * / %`)
- Comparison operators (`== != < > <= >=`)
- Logical operators (`&& || !`)
- Bitwise operators (`& | ^ ~ << >>`)
- Unary operators
- Ternary operator (optional but useful)

2. Data types

Primitive and composite types required to represent compiler structures.

Primitive types:

- `bool`
- `char`
- `int`
- `unsigned int`
- fixed width integers (`u8 u16 u32 u64`)
- signed integers (`i8 i16 i32 i64`)
- `float`
- `double`
- `string`

Compound types:

- `struct`
- arrays
- dynamic arrays
- lists
- maps
- hashmaps
- tuples (optional)
- enums
- tagged unions / variants
- optional / nullable types

Low level types:

- pointer types
- references
- raw byte buffers
- slices / views

3. Memory management

A compiler allocates thousands of AST nodes and objects.

Required capabilities:

- stack allocation
- heap allocation
- dynamic allocation (`new` / `malloc` equivalent)
- deallocation (`free` / `delete`) or garbage collection
- allocator abstraction
- memory copying
- memory comparison
- memory resizing
- zero initialization
- pointer arithmetic

4. Control flow

Essential for implementing parsing and analysis.

- `if`
- `else`
- `switch`
- `match` / pattern matching (optional)
- `for` loops
- `while` loops
- `do while`
- `break`
- `continue`
- `return`
- recursion support

5. Functions

Compiler code is heavily modular.

- function declarations
- function definitions
- parameters
- return values
- multiple parameters
- pass by value
- pass by reference
- function overloading (optional)
- inline functions
- recursion

6. Type system capabilities

Needed for semantic analysis.

- type declarations
- type checking
- type inference (optional)
- casting
- `static_cast` style conversion
- implicit conversion
- explicit conversion
- generics or templates (optional but useful)

7. Modules and program organization

A compiler must be split across many files.

- modules or namespaces
- import / include system
- file level compilation units
- symbol visibility (`public` / `private`)
- header or interface system
- dependency resolution

8. Preprocessing capability

Since you already support this.

- conditional compilation
- macros (optional)
- compile time constants
- include directives
- comment parsing

9. String manipulation

Critical for lexing and parsing.

- string creation
- string concatenation
- substring extraction
- string comparison
- string length
- character indexing
- string iteration
- string to number conversion
- number to string conversion

10. Character handling

Lexers operate at the character level.

- ASCII handling
- unicode support (optional but common)
- character classification
- digit detection
- whitespace detection
- identifier detection

11. Collections and containers

Used throughout the compiler.

- dynamic arrays
- vectors
- linked lists
- hash maps
- sets
- stacks
- queues
- iterators
- container resizing

12. File I/O

A compiler must read source files and write binaries.

- open file
- close file
- read file
- write file
- append file
- binary read
- binary write
- file existence check
- file size query

13. Directory and filesystem access

Required for module loading.

- directory listing
- path manipulation
- working directory access
- relative path resolution
- file timestamp query

14. Command line interaction

Compilers take arguments.

- access program arguments
- environment variables
- process exit codes
- stdout printing
- stderr printing

15. Error handling

Compilers must recover from errors.

- error reporting
- error propagation
- diagnostics
- stack traces (optional)
- result / option types
- exceptions (optional)

16. Lexer infrastructure

Tokenizing source code.

- token type definitions
- token structures
- token streams
- lexical scanning
- whitespace skipping
- comment skipping
- numeric literal parsing
- string literal parsing
- identifier parsing
- keyword recognition

17. Parser infrastructure

Building syntax trees.

- recursive descent parsing
- precedence parsing
- AST node types
- AST tree structures
- parser state
- error recovery
- lookahead tokens

18. AST representation

The internal structure of programs.

- AST node base type
- expression nodes
- statement nodes
- declaration nodes
- type nodes
- module nodes
- tree traversal
- visitor pattern (optional)

19. Symbol tables

Used during semantic analysis.

- symbol table structure
- scope stack
- variable lookup
- function lookup
- type lookup
- namespace resolution

20. Semantic analysis

Verifying program correctness.

- type checking
- variable resolution
- function resolution
- overload resolution
- constant evaluation
- scope validation
- control flow analysis

21. Intermediate representation (optional but common)

Most compilers translate AST into IR.

- IR instruction structures
- basic blocks
- control flow graph
- SSA form (optional)

22. Code generation

Transforming IR or AST into machine instructions.

Capabilities needed:

- instruction encoding
- register allocation
- stack frame generation
- calling conventions
- function prologues
- function epilogues
- branching instructions
- arithmetic instructions

23. Binary generation

To produce executable files.

- object file generation
- relocation entries
- symbol tables
- section creation
- binary buffers

For Windows specifically:

- PE header generation
- section tables
- import tables
- entry point specification

24. Linker interaction

Two approaches exist.

Option A:
Your compiler generates assembly and invokes an external assembler.

Option B:
Your compiler generates object files and invokes a linker.

Required abilities:

- spawn processes
- pass arguments to external tools
- capture exit codes

25. Platform interface

For Windows compilation.

- process execution
- environment variables
- system calls
- thread creation (optional)

26. Build orchestration

The compiler must build itself.

- dependency graph
- incremental compilation (optional)
- module ordering
- build scripts

27. Standard library foundation

Your language needs a minimal stdlib.

Typical components:

- string utilities
- container library
- filesystem utilities
- memory utilities
- error utilities
- formatting utilities

28. Bootstrapping

Self hosting requires staged compilation.

Stage process:

- Stage 1: Compiler written in C++ or another language
- Stage 2: Compile EreLang compiler written in EreLang
- Stage 3: Use EreLang compiler to compile itself
- Stage 4: Output `erelang.exe`

29. Deterministic builds

For a stable compiler.

- deterministic hashing
- reproducible builds
- stable symbol ordering

30. Performance infrastructure

Compilers must handle large codebases.

- fast string storage
- arena allocators
- memory pools
- hash tables
- caching

Each helper integrates with the network debug logger, so the report is also written to the log file when tracing is enabled.

## Plugins (.elp)

Erelang looks for plugins in a `plugins` directory that sits next to `erelang.exe` (or your compiled app). The runtime creates the folder automatically if it is missing, so you can just drop a plugin package inside `plugins/YourPlugin/project.elp`.

A plugin manifest is XML-like:

```
<plugin>
  <erelang_manifest>
    <id>com.example.plugin</id>
    <name>Example Plugin</name>
    <version>1.0.0</version>
    <author>Author Name</author>
    <target>erelang>=1.0.0</target>
    <description>Extends Erelang with new GUI events and syntax helpers.</description>
  </erelang_manifest>

  <dependencies>
    <require>core.gui</require>
    <require>runtime.debug</require>
  </dependencies>

  <content>
    <include>src/ExampleEvent.0bs</include>
    <include>assets/gui_icons.json</include>
  </content>

  <hooks>
    <onLoad>plugin_init</onLoad>
    <onUnload>plugin_shutdown</onUnload>
  </hooks>
</plugin>
```

- When you import a glob of plugin manifests, the runtime also exposes metadata for every discovered plugin. Example:

```
import "/plugins/*/project.elp" as plugin

action main {
  print "loaded=" + plugin.count
  print "slugs=" + plugin.slugs
  let p = plugin["error_handler"]
  print p.name + " v" + p.version
}
```

  - `plugin.count` and `plugin.slugs` give you a quick overview (the list of slugs is comma-separated).
  - `plugin[slug]` resolves to a `Plugin` object with fields: `id`, `slug`, `name`, `version`, `author`, `target`, `description`, `dependencies`, `base_directory`, `manifest_path`, `on_load`, `on_unload`.
  - All plugin metadata is available during `onLoad`, `onStart`, and any actions or hooks—regardless of whether the script runs from the CLI or a window loop.
  - `.core` files listed in `<content>` are parsed as key/value pairs. Use `plugin_core(slug, "file:key")`, `plugin_core_files(slug)`, and `plugin_core_keys(slug, file)` to read them at runtime.
  - This repository includes working samples under `plugins/example/` (metadata demo) and `plugins/error_handler/` (real error logging). Copy them next to `erelang.exe` (for example, `build/plugins/<slug>`) to experiment with lifecycle hooks and metadata.

- Every `<include>` path is resolved relative to the plugin folder. Script files (`.0bs`, `.erelang`, `.obsecret`) are parsed before the main program so their actions, hooks, and entities become available immediately. Non-script assets stay on disk for your own loaders.
- `<dependencies>` is advisory today; the runtime records the requested feature names and can warn if a required module is missing in the future.
- The runtime runs `<hooks><onLoad>` once before `onStart` and `<hooks><onUnload>` after `onEnd` (or `onExit`). Those actions can mutate globals just like any other startup code.
- Plugins also run when you call `Runtime::run_single_action` for window loops—the first frame triggers `onLoad` automatically.

Ship your plugin by copying its folder (with scripts, assets, and `project.elp`) into the application's `plugins` directory.

## Lists and Dictionaries

Simple dynamic containers are available as built-ins.

```
action main {
  let xs = list_new()
  list_push(xs, "a")
  list_push(xs, "b")
  print "len=" + list_len(xs)
  print list_get(xs, 0)

  let d = dict_new()
  dict_set(d, "name", "Erelang")
  if (dict_has(d, "name")) {
    print dict_get(d, "name")
  }
}
```

- Lists: `list_new`, `list_push(list, value)`, `list_get(list, index)`, `list_len(list)`
- Dicts: `dict_new`, `dict_set(dict, key, value)`, `dict_get(dict, key)`, `dict_has(dict, key)`

## Windows GUI: Two Ways

There are two ways to build a GUI on Windows.

1) Low level primitives — full control

```
action main() {
  let win = win_window_create("My App", 360, 240)
  win_label_create(win, "Welcome", 10, 50, 220, 20)
  win_button_create(win, "ok", "OK", 10, 10, 80, 28)
  win_show(win)
  win_loop("onEvent")
}

action onEvent() {
  if (id == "ok") {
    win_message_box("", "Clicked OK")
  }
}
```

2) Clean wrapper — simpler API layered on primitives

Import `std/ui` and use the `Gui` entity.

```
import "std/ui"

action main() {
  let ui = new Gui("UI Demo", 420, 240)
  ui.addLabel("Name:", 10, 15, 60, 20)
  ui.addTextBox("name", 70, 12, 200, 22)
  ui.addButton("ok", "OK", 280, 10, 60, 24)
  ui.show()
  ui.loop("onEvent")
}

action onEvent() {
  if (id == "ok") {
    let name = win_get_text(win, "name")
    print "OK clicked, name=" + name
  }
}
```

Notes

- During event callbacks the runtime sets `id` to the control identifier and sets `win` to the current window handle string, so you can call `win_get_text(win, ...)` without having to store the handle yourself.
- UI is themed with Segoe UI font and visual styles.

## Standard Library Imports

The loader resolves relative paths and a simple `std` prefix. For example:

```
import "std/ui"
```

The repository provides `examples/std/ui.0bs` and `examples/std/gui.0bs`.

## Debugging

You can run with a debug driver using `--debug`. The driver adds helper actions like `bp(label)` which pause execution.

```
erelang .\examples\debug_demo.0bs --debug
```

In scripts:

```
import "./lib/debugger.elan"

action main() {
  print "before"
  bp("check")
  print "after"
}
```

## CLI Reference

```
erelang --help
erelang <path\to\file.0bs> [--debug]
erelang --compile <path\to\file.0bs> [--output <path\to\out.exe>]
erelang --make-debug [--output <path\to\debug.exe>]
```

## Tips

- To run `erelang` without `./` add your `build` folder to the PATH. The setup above already handled that.
- If you see old looking UI, rebuild after pulling updates to get manifest and visual styles.

## Roadmap

- Expression statements so you can write call expressions as standalone statements
- More GUI controls and events
- Better diagnostics and error ranges
- Cross platform stubs for non Windows environments
