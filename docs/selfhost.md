# Erelang mini-compiler frontend

Small compiler pipeline written in Erelang (`.elan`): lex, parse, resolve, typecheck, lower to IR, run via IR interpreter.

Self-host of the full `compiler/*.elan` corpus is **paused**. This tree is a fixture-driven frontend only.

## How to run

From the repo root, with an existing binary:

```text
build\bin\Debug\erelang.exe compiler/main.elan
```

No CMake rebuild required for this frontend (unless host memory builtins changed). Does not call LLVM and does not emit x86.

## Pipeline

`compiler/main.elan` drives:

1. **Lex** (`lexer.elan`) — source string to `TokenList` (list or buffer-backed cursor)
2. **Parse** (`parser.elan`) — tokens to `Stmt` AST (`parse`) or `Program` of decls (`parse_program`)
3. **Resolve** (`resolver.elan`) — scopes, shadowing, unknown-name diagnostics
4. **Typecheck** (`typechecker.elan`) — `int` / `bool` / `string` for the subset
5. **IR** (`ir.elan` + `codegen.elan`) — stack IR + interpreter

## Subset covered

- Integer / bool / string literals; unary `-`
- `+ - * /` with precedence and parentheses; comparisons `== != <`
- `let` bindings; typed locals; assignment and field assign
- `if` / `else`, `while`, `return`, `match` / `case`
- `print` of `int` or string literal
- Calls and member access (postfix)
- Explicit generic syntax (parsed; erased in typecheck/codegen)
- Top-level `public action`, `enum`, `struct` via `parse_program`

## Fixtures

| Fixture | Expect |
|---------|--------|
| happy.mini | print **10** |
| ir_prec.mini | **9** |
| ir_while.mini | **10** |
| ir_else.mini | **2** |
| ir_arith.mini | **13** |
| ir_call.mini | **13** |
| program_slice.elan | parse ok for action/enum/struct/… |

## Limits (not self-host)

- No recursive self-compile of `compiler/*.elan`
- No LLVM / native codegen from this frontend
- Generics erased (no monomorphization)
- Entities, async, channels out of scope

## Files

| File | Role |
|------|------|
| `compiler/token.elan` | tokens |
| `compiler/lexer.elan` | `lex(source)` |
| `compiler/ast.elan` | `Expr`, `Stmt`, `Decl`, `Program` |
| `compiler/parser.elan` | `parse` / `parse_program` |
| `compiler/diagnostic.elan` | diagnostics |
| `compiler/resolver.elan` | name resolution |
| `compiler/typechecker.elan` | type checking |
| `compiler/ir.elan` | IR types |
| `compiler/codegen.elan` | lower + interpret |
| `compiler/main.elan` | fixture driver |
| `compiler/fixtures/*` | sample sources |
