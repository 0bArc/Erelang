# Erelang Tutorial

This walkthrough guides you from cloning the repository to building native binaries, writing scripts, using the Windows GUI helpers, and packaging your own executable. Every step references concrete files in this repo so you know where each feature lives.

## 1. Prerequisites

* Windows 10 or later (the toolchain and GUI helpers are Windows-first today).
* CMake with either Ninja or MinGW generators (the quick-start paths assume `-G "MinGW Makefiles"`).
* A working C++ toolchain (MinGW-w64 or MSVC). The GUI build launcher (`src/build_launcher.cpp`) defaults to MinGW paths — tweak commands if you favour MSVC.
* No PowerShell requirement for core configure/build targets.
* VS Code if you plan to use the bundled extension under `vscode/erelang-vscode`.

Optional extras:

* Node.js + npm for building and packaging the VS Code extension.
* Git for source control (the checked-in tree ships without `.git/`).

## 2. Configure and build the toolchain

From the repository root run:

```pwsh
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

These targets produce the main executables:

* `build/erelang.exe` – CLI runner implemented in `src/erelang_main.cpp`.
* `build/obc.exe` – Lightweight compiler driver from `src/main.cpp`.
* `build/build.exe` – Dark-themed GUI orchestrator defined in `src/build_launcher.cpp`.

Prefer a GUI to manage builds? Launch `build/build.exe` and use the **Build/Rebuild** buttons; it wraps the same CMake commands with log streaming.

## 3. Explore the language structure

Before coding, skim `docs/erelang.md` (repository overview), `docs/guide.md` (language guide), and `implemented.manifest` (feature checklist). They map declarations, statements, and attributes to the actual implementation in `src/parser.cpp`, `src/typechecker.cpp`, and `src/runtime.cpp`.

Key takeaways:

* Scripts use curly braces and live in `.0bs` files.
* Top-level constructs include `action`, `entity`, `hook`, `global`, `import`, and `run` directives.
* Attributes like `@strict` or `@entry("main")` are captured in the AST (`Attribute` structs in `parser.hpp`).
* Built-ins are declared in `TypeChecker::init_builtins` and implemented under `src/builtins/`.

action main {
## 4. File scaffolding and your first script

Every `.0bs` file should start with the `@erelang` directive so the parser knows it is targeting the braces-based syntax. When you have a primary entry point, add `@entry("main")` (or any other action name) and make that action explicitly `public` or `private` — even outside `@strict` mode we keep the visibility explicit for clarity.

Create `tutorial/hello.0bs`:

```
@erelang
@entry("main")

public action main {
  print "Hello from Erelang"
  sleep 250ms
  print "Done!"
}

run main
```

`lexer.cpp` tokenises duration literals such as `250ms`; `parser.cpp` lowers them to integer milliseconds; `runtime.cpp` performs the actual sleep.

Run it with:

```pwsh
./build/erelang.exe tutorial/hello.0bs
```

`erelang.exe` automatically honours the `run` directive at the bottom of the file and exits with status code `0` on success.

## 5. Variables, expressions, and interpolation

action mathDemo {
Add `tutorial/basics.0bs`:

```
@erelang
@entry("main")

public action mathDemo {
  let x = 2
  let y = 3
  print "x + y = {x + y}"
  print "x * 10 = {x * 10}"
  print "Equality? {x == 2}"
}

public action main {
  mathDemo()
}

run main
```

Observations:

* `parser.cpp` parses arithmetic via `parse_additive`/`parse_multiplicative`; expressions use the `BinaryExpr` / `UnaryExpr` variants defined in `parser.hpp`.
* `Runtime::eval_string` handles string interpolation (`{expr}`) by re-evaluating the embedded expression.
* `typechecker.cpp` infers `int`, `string`, and `bool` for basic operations and issues `TC030` if you redeclare a variable in the same scope.

Run the file to see interpolated output.

## 6. Branching, loops, and switches

Climb through control flow constructs implemented in `parser.cpp` and guarded by the type checker:

action branching(kind: string) {
```
@erelang
@entry("main")

public action branching(kind: string) {
  if (kind == "a") {
    print "branch a"
  } else {
    print "other"
  }

  switch kind {
    case "a" { print "matched a" }
    case "b" { print "matched b" }
    default   { print "fallback" }
  }

  let count = 0
  while (count < 3) {
    print count
    count = count + 1
  }

  for (let i = 0; i < 3; i = i + 1) {
    print "loop {i}"
  }
}

public action main {
  branching("a")
}

run main
```

`StmtChecker::check_stmt` enforces boolean conditions (`TC060`–`TC062`) and warns about unreachable code (`TC070`).

## 7. Parallel work and threads

`parallel { ... }` pushes new worker threads into `ExecContext::threads` (see `runtime.cpp`). `wait all` is parsed but the runtime implementation remains on the TODO list (tracked in `implemented.manifest`). Until it lands, use the explicit thread built-ins wired in `src/builtins/threads.cpp`:

action main {
```
@erelang
@entry("main")

public action main {
  let handle = thread_run("worker")
  print thread_state(handle)
  thread_join(handle)
}

private action worker {
  sleep 500ms
  print "done"
}

run main
```

Thread handles are strings like `thread:1`; `TypeChecker::init_builtins` sets their arity and return types.

## 8. Data stores, binary buffers, and regex

Experiment with the helpers in `src/builtins/data.cpp`, `binary.cpp`, and `regex.cpp`:

action main {
```
@erelang
@entry("main")

public action main {
  let store = data_new()
  data_set(store, "name", "Erelang")
  print data_get(store, "name")

  let buf = bin_new()
  bin_push_u8(buf, 72)
  bin_push_u8(buf, 105)
  print bin_hex(buf)

  print regex_find("(Erelang)", "Hello Erelang")
}

run main
```

Handles (`data:1`, `bin:2`) map back to in-memory state via globals declared in `runtime_internals.hpp` and defined in `runtime.cpp`.

## 9. Networking and downloads

`src/builtins/network.cpp` wraps WinHTTP and HLS helpers:

action main {
```
@erelang
@entry("main")

public action main {
  let body = http_get("https://httpbin.org/get")
  print body

  let ok = http_download("https://example.com/index.html", "downloaded.html")
  print "Downloaded? {ok}"
}

run main
```

The same builtin file exposes `hls_download_best` and network maintenance helpers such as `network.ip.flush()` and `network.ip.renew()` (see `examples/download_best_hls.0bs` for HLS and the guide for IP tools).

## 10. Monitoring files

`src/builtins/monitor.cpp` runs background watcher threads guarded by the policy subsystem:

action main {
```
@erelang
@entry("main")

public action main {
  let handle = monitor_add("tutorial/hello.0bs", "hello")
  monitor_set_interval(handle, 1000)
  sleep 5s
  print monitor_info(handle)
  monitor_remove(handle)
}

run main
```

Use `policy.cfg` to disable or restrict monitoring in production.

## 11. Windows GUI primitives

`runtime.cpp` implements low-level `win_*` helpers. A minimal window looks like:

action main {
action onEvent {
```
@erelang
@entry("main")

public action main {
  let win = win_window_create("Demo", 360, 240)
  win_label_create(win, "Welcome", 40, 20, 200, 24)
  win_button_create(win, "ok", "OK", 40, 60, 120, 32)
  win_show(win)
  win_loop("onEvent")
}

private action onEvent(win: string, id: string) {
  if (id == "ok") {
    win_message_box(win, "Clicked", "You pressed OK")
    win_close(win)
  }
}

run main
```

During event callbacks the runtime injects `id` (control identifier) and `win` (window handle string); declare them as action parameters (e.g. `onEvent(win: string, id: string)`) so the type checker recognises them.

## 12. Packaging to an executable

`erelang --compile` bundles your script, its imports, and runtime bits into a standalone executable:

```pwsh
./build/erelang.exe --compile tutorial/hello.0bs --output tutorial/hello.exe
```

`src/erelang_main.cpp` writes out `manifest.erelang` with dependency metadata and supports `--static` / `--dynamic` toggles for linking.

## 13. Debugging support

`--debug` loads the driver from `examples/lib/debugger.elan` (with legacy fallback to `examples/debug/debug.0bs`), adding helpers like `bp("label")` and `trace(expr)`:

```pwsh
./build/erelang.exe examples/test.0bs --debug
```

Browse `src/obs_main.cpp` (debug path) and `examples/lib/debugger.elan` to see how hooks are wired.

## 14. Policy controls

`policy.cfg` defaults to `default_deny=false` and raises quotas (`max_threads`, `max_list_items`). Harden deployments by:

1. Setting `default_deny=true`.
2. Listing essential builtins under `allow=`.

`PolicyManager::load` (implemented in `src/policy.cpp`) reads the file once, and every builtin dispatcher checks `PolicyManager::instance().is_allowed(name)` before executing.

## 15. VS Code workflow

The extension in `vscode/erelang-vscode` provides syntax highlighting, completions, run/compile commands, and code lenses.

1. Install and build:
   ```pwsh
   cd vscode/erelang-vscode
   npm install
   npm run compile
   ```
2. Press **F5** (Launch Extension) to load it in a new VS Code instance.
3. Use command palette entries **erelang: Run File**, **Run with Args**, and **Compile File** — they shell out to `erelang.exe` using safe quoting (see `client/src/extension.ts`).

`tools/build-and-update-extension.ps1` automates `npm run compile`, packages a `.vsix`, and installs it using the `code` CLI.

## 16. Going further

* Review `implemented.manifest` whenever you add syntax or runtime features so the checklist stays accurate.
* Explore `examples/` for complete programs: HTTP downloads, window demos, unit maths, network diagnostics, polymorphic identifier tests.
* Embed the runtime by linking against the C ABI defined in `include/erelang/cabi.h` (`src/cabi_exports.cpp` provides `ob_run_file`).

Happy scripting! Keep this tutorial, the guide, and the overview in sync as the language evolves.
