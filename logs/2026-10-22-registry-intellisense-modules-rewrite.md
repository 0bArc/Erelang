# 2026-10-22: builtin registry, manifest-driven IntelliSense, modular runtime, type examples

## What changed
- One builtin registry (`src/runtime/builtin_registry.cpp`) feeds imports, the typechecker, `erelang --dump-builtins [--json]` and the VS Code manifest. `tools/check_builtin_registry.py` checks it against the implementations.
- VS Code extension reads a generated manifest (`npm run gen`, bundled in the vsix) and refreshes it from the installed exe at startup. Member completion needs a known receiver type; `re.*` and `ai.*` appear only after the matching include.
- Modules register themselves through `module_registry.hpp`; `--compile` references only the modules a program imports.
- Process handles (`proc:N`: wait, alive, kill, pid, read, read_err, write), top-level JSON arrays, `json.parse`, Docker plugin (`plugins/docker`).
- Rewrites: actions.cpp split into exec_stmt/exec_decl/exec_calls, math, data, optimizer, value formatting, lexer and parser fixes (`^=`, ranges, ternary).
- Fixed: `Point b = a;` baked the source struct into a dict handle, so the next `a.method()` returned nothing.
- New `examples/types/` (8 self-checking files plus `run.ps1`), published in the docs under Examples.

## Verification
- tests/run.ps1: 78 pass, 0 fail. examples/types/run.ps1: 8 ok.
- mkdocs build: no warnings besides git-revision notices.

## Open
- `--compile` size comparison was not cleanly measured (static libstdc++ inflated sizes).
- Remaining docs pages (performance, log, pipe, json, cli, plugins) and a deeper bytecode/memory speed pass are not done.
