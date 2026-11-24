---
name: language-feature
description: End-to-end checklist for adding or changing Erelang syntax, types, semantics, builtins or modules across lexer, parser, typechecker, runtime, imports, codegen, editor and docs. Use for any change to the language surface or a builtin.
---

# Language feature changes

Trace each feature end to end before modifying it. Depending on the change, review:

1. Tokenization in `src/lexer.cpp`.
2. Parsing and AST types in `src/parser.cpp` and `include/erelang/`.
3. Static semantics and diagnostic messages in `src/typechecker.cpp`.
4. Execution in `src/runtime/eval.cpp`, `src/runtime/actions.cpp`, `src/runtime/exec_*.cpp`, or other runtime files.
5. AST serialization, optimization, IR, and codegen if those stages consume the construct.
6. Editor grammar, snippets, completions, and diagnostics in `erevos-language/`.
7. Documentation in `docs/pages/` and executable positive and negative examples.

A parser feature is incomplete if it cannot be typechecked or run through the intended execution path. Do not add permissive `any` fallbacks for precise types. Error cases need actionable, consistent diagnostics.

## Builtins and modules

For a new or changed builtin, verify applicable touch points:

- Implementation under `src/runtime/builtins/` or the existing core builtin path.
- Registration in `src/runtime/builtin_registry.cpp` / `src/runtime/module_registry.cpp`.
- Dispatch in `src/runtime/builtins.cpp` and any handle dispatch in `src/runtime/actions.cpp`.
- Import registration and aliases in `src/runtime/imports.cpp`.
- Exact signatures, argument checks, and return types in `src/typechecker.cpp`.
- Source registration in `CMakeLists.txt` for each relevant library configuration.
- Module docs, examples, tests, and editor completions (`tools\gen-extension-manifest.ps1`, then `python tools\check_builtin_registry.py`).

Keep names, arity, types, aliases, permissions, and error semantics aligned. Builtins that require explicit imports must not become accessible through an unintended alias or dispatch path.

## Documentation

`docs/pages/` is the intended language reference, but existing source and tests take precedence when documentation has drifted. Edit the relevant established page, not a new duplicate, and list new pages in `docs/mkdocs.yml`. Keep examples executable against the implemented syntax. Distinguish shipped behavior, experimental behavior, and future proposals. Never invent an Erelang API or imply a C++ implementation is automatically exposed to scripts.

Respect the `ERELANG_EXPERIMENTAL` build option and do not present experimental functionality as stable without approval.
