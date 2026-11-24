# Erelang Development Instructions

These instructions apply to AI-assisted changes in this repository. Build correct, maintainable language features with small, reviewable diffs. Do not optimize for the amount of code written.

## Project

Erelang is an Apache 2.0 licensed scripting language implemented in C++20. Its interpreter runs `.elan` scripts on Windows, Linux and macOS; every change must keep all three working. It also has compiler tooling, builtins, optional runtime configurations, and a VS Code extension. Read `README.md`, `CONTRIBUTING.md`, relevant `docs/pages/` pages, and existing tests before changing behavior.

The lexer, parser, typechecker, runtime, imports, compiler, documentation, and editor support form one language implementation. Keep affected layers consistent. Do not call a feature finished because only one part builds.

## Skills

Load the matching skill from `.claude/skills/` before starting the task:

| Task | Skill |
| --- | --- |
| Any change that adds, moves or deletes files; temp files; logs | `repo-hygiene` |
| Building, running, testing, extension, release config | `build-and-test` |
| New or changed syntax, types, builtins, modules | `language-feature` |
| Any C++ edit in `src/`, `include/`, `experimental/` | `cpp-quality` |

## Repository map

| Responsibility | Location |
| --- | --- |
| Lexer and parser | `src/lexer.cpp`, `src/parser.cpp` |
| Typechecker | `src/typechecker.cpp` |
| Optimizer, IR, x64 codegen | `src/optimizer.cpp`, `src/ir.cpp`, `src/codegen_x64.cpp` |
| Public interfaces and AST | `include/erelang/` |
| Runtime execution | `src/runtime/core.cpp`, `src/runtime/eval.cpp`, `src/runtime/actions.cpp`, `src/runtime/exec_*.cpp` |
| Builtin registry, dispatch and imports | `src/runtime/builtin_registry.cpp`, `src/runtime/builtins.cpp`, `src/runtime/imports.cpp` |
| Individual builtins | `src/runtime/builtins/` |
| Runner CLI | `src/obs_main.cpp` (`erelang_runner`, output `erelang.exe`) |
| Compiler CLI | `src/main.cpp` (`obc`) |
| Build system | `CMakeLists.txt`, `CMakePresets.json`, `tools/cmake/` |
| Socket and TLS portability | `src/runtime/builtins/net_platform.hpp` |
| CI (Linux, macOS) | `.github/workflows/ci.yml` |
| Reference documentation | `docs/pages/` (MkDocs, `docs/mkdocs.yml`) |
| Examples, benchmarks, conformance tests | `examples/`, `examples/bench/`, `tests/` |
| VS Code extension | `erevos-language/` |
| Plugins | `plugins/` |
| Experimental code | `experimental/` |
| Work logs | `logs/` |

The files in `compiler/*.elan` are separate language-side compiler work. Do not confuse them with the C++ frontend.

## Clean files

The root stays small. It holds only `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `CONTRIBUTING.md`, `LICENSE`, `CLAUDE.md`, `.gitattributes`, the git and editor config folders, and the source folders above. Do not add files or folders to the root.

- Build only through the presets: `build/debug` and `build/release`. Never create `build-*`, `build_*` or other build folders.
- Scratch files go in the session scratchpad, never in the repository. Delete anything temporary you created before finishing.
- Do not commit one-off scripts, `*.old`, `*.bak`, logs, dumps or generated output.
- Every task that changes a file gets a log in `logs/` (see `repo-hygiene`).

## Workflow

1. Understand the requested behavior and inspect all relevant implementations, callers, tests, and docs.
2. Identify the smallest complete change. Prefer extending an established path over duplicating behavior.
3. Implement it across every affected layer. Do not silently bypass validation or permissions.
4. Add focused tests for correct behavior and expected failures.
5. Build and run relevant tests. Run the broader conformance suite when feasible.
6. Update the corresponding docs and editor support if the language surface changed.
7. Review the diff for unrelated changes, dead code, stale files, warnings, and regressions.
8. State exactly what was changed, tested, and not verified. Never invent results.

## Completion criteria

Before finishing, verify the following wherever relevant:

- The change matches the request and has no unrelated refactoring.
- Parser, typechecker, runtime, imports, optimizer, codegen, and editor are consistent.
- Ownership, permissions, and error handling remain correct.
- Positive and negative regression coverage exists.
- Affected targets build and relevant tests pass, or any limitations are disclosed.
- Docs and examples match actual behavior.
- No temporary stubs, needless abstractions, extra dependencies, version bumps, generated artifacts, or stray files were introduced.

## Final response format

Keep the final report short and factual:

- **Changed:** Files and behavior changed.
- **Verified:** Exact build and test commands actually run and their outcomes.
- **Not verified:** Checks not run, with reasons.
- **Notes:** Only relevant compatibility risks, open issues, or requested follow-up work.

Never claim an unexecuted build or test succeeded.
