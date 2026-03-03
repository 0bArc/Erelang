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
import "./lib/debugger.0bs"

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
