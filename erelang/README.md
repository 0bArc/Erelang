# Erelang-in-Erelang Bootstrap

This folder starts the self-hosting migration.

## Layout
- `compiler.elan` — pipeline entrypoint and native-provider smoke runner.
- `src/ast.elan` — AST/domain structs (`CompileResult`, constructors).
- `src/core.elan` — core language helpers (`nullptr`, null checks).
- `src/lexer.elan` — lexical stage.
- `src/parser.elan` — parse stage.
- `src/symboltable.elan` — symbol/scope stage scaffold.
- `src/typechecker.elan` — semantic validation stage scaffold.
- `src/ir.elan` — lowering stage.
- `src/optimizer.elan` — optimization stage scaffold.
- `src/codegen.elan` — code emission stage.
- `src/runtime.elan` — native AST execution helper.
- `examples/hello.elan` — sample input target.

The old `modules/` folder is kept as legacy bootstrap scaffolding.

## Run
From repo root:

- `build/bin/Release/erelang.exe erelang/compiler.elan`

This now runs a native-provider bootstrap flow in `.elan` (parse/typecheck/execute AST) for the supported smoke subset.

Current native smoke scenarios:
- `print "Hello from self-host bootstrap"`
- `print 1 + 2`
- `repeat 5 { print "Hi" }`
- `Struct` + `Impl` + `call` (self-host subset)

Impl subset syntax (single-line forms):
- `Struct Weapon`
- `Impl Weapon.describe { print "Impl says hi" }`
- `call Weapon.describe`
- `repeat 3 { call Weapon.describe }`

Sample file: `erelang/examples/impl_demo.elan`

Run the pure self-host path (no C++ Impl parser changes):
- `build/bin/Release/erelang.exe erelang/examples/impl_demo_runner.elan`

Note:
- `build/bin/Release/erelang.exe erelang/examples/impl_demo.elan` still uses the core C++ parser directly and will fail on `impl` until core grammar is updated.

Legacy transpile/emit scaffolding remains in `src/ir.elan` + `src/codegen.elan` for transition work.

## Goal
Incrementally replace C++ front-end/runtime responsibilities with `.elan` implementations while keeping C++ as host runtime during transition.

Near-term roadmap:
1. Expand parser to expression grammar (precedence + parentheses).
2. Flesh out `symboltable.elan` and `typechecker.elan` with real checks.
3. Add IR instruction model and optimizer passes.
4. Add CLI build driver for project-wide `.elan` compilation.
