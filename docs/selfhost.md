# Self-hosted Erelang compiler frontend

A small compiler pipeline written in Erelang (`.elan`) that lexes, parses, resolves, typechecks, lowers to IR, and runs a tiny language via an IR interpreter.

## How to run

From the repo root, with an existing binary:

```text
build\bin\Debug\erelang.exe compiler/main.elan
```

No CMake rebuild is required for this frontend (unless host memory builtins changed). It does not call LLVM and does not emit x86.

## Pipeline

`compiler/main.elan` drives:

1. **Lex** (`lexer.elan`) — source string to `TokenList` (comments, strings, punctuation; Keyword+text; collect diagnostics, do not abort on first bad char)
2. **Parse** (`parser.elan`) — tokens to `Stmt` AST (`parse`) or `Program` of decls (`parse_program`)
3. **Resolve** (`resolver.elan`) — scopes, shadowing, unknown-name diagnostics
4. **Typecheck** (`typechecker.elan`) — `int` / `bool` / `string` for the subset
5. **IR** (`ir.elan` + `codegen.elan`) — stack IR: int ops, `CONST_STR`, `CALL`, `PRINT` / `PRINT_STR`, jumps, `RETURN`
6. **Run** — IR interpreter in `codegen.elan`

Diagnostics use:

```text
error: expected ')'
  --> main.elan:14:23
```

## Subset covered

- Integer / bool / string literals; unary `-`
- `+ - * /` with precedence and parentheses; comparisons `== != <`
- `let` bindings; typed locals (`int i = 0`, `Point p;`); assignment and field assign
- `if` / `else`, `while`, `return`, `match` / `case`
- `print` of `int` or string literal
- Calls and member access (postfix)
- Explicit generic syntax: `action identity<T>(value: T): T`, `identity<int>(1)`, and `Option<int>` type references
- Top-level `public action`, `enum`, `struct` via `parse_program`
- Lex / parse / type errors as diagnostics (no crash)

Fixtures under `compiler/fixtures/` (also embedded in `main.elan`):

| Fixture | Expect |
|---------|--------|
| happy.mini | print **10** |
| ir_prec.mini | **9** |
| ir_while.mini | **10** |
| ir_else.mini | **2** |
| ir_arith.mini | **13** |
| ir_call.mini | call + string print → **13** |
| selfhost_slice.elan | parse ok for action/enum/struct/string/call/member/match/return/typed locals |

## Memory (host)

Heap objects use the host model in [memory.md](memory.md): `alloc`/`free`, `heap<T>`, `shared<T>`, `buffer<T>`. Debug: `mem_stats()`, `mem_alive()`. Mini-compiler AST is still value enums; later codegen can lower to the same API.

## Runtime boundary and remaining limits

- `fs.read`, `fs.write`, and `fs.exists` are runtime-backed through `#include <builtin/fs> as fs`; C++ host preprocessing remains compatibility fallback for full source loading.
- Mini compiler reads source files through `fs.read` in `parse_file_expect`, proving Erelang-side filesystem access. It does not yet recursively expand arbitrary include graphs.
- Generic parameter and argument syntax is parsed and preserved only as source-level syntax. Mini typechecking and codegen erase parameters, so no inference or monomorphization exists yet.
- Compiling full `compiler/*.elan` with itself still needs recursive include expansion, method calls like `string.len(...)`, full attribute/`run` wiring, and exhaustive host syntax.
- LLVM or native codegen
- Entities, async, channels

## Files

| File | Role |
|------|------|
| `compiler/token.elan` | `Tok` (Keyword+text), `Token`, helpers |
| `compiler/lexer.elan` | `lex(source)` |
| `compiler/ast.elan` | `Expr`, `Stmt`, `Decl`, `Program` |
| `compiler/parser.elan` | recursive-descent `parse` / `parse_program` |
| `compiler/diagnostic.elan` | `Diagnostic` + formatter |
| `compiler/resolver.elan` | name resolution |
| `compiler/typechecker.elan` | int/bool/string checking |
| `compiler/ir.elan` | IR instruction types |
| `compiler/codegen.elan` | lower AST → IR + interpreter |
| `compiler/main.elan` | pipeline driver + fixtures |
| `compiler/fixtures/*` | sample sources |
