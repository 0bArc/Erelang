# 2026-11-24 Optional semicolons

## What was wrong

Every simple statement had to end with `;` (`parse_block` in `src/parser.cpp` called
`expect(TokenKind::Semicolon)`; top-level `global`, `extern`, `import` and `run` did the same).
A newline gave `Expected ; (near '<newline>')`.

## What changed

- `src/parser.cpp`, `include/erelang/parser.hpp`: new `Parser::expect_statement_end()` accepts `;`,
  or leaves a newline, `}` or end of input in place. Used for block statements and the top-level
  `global`, `extern`, `import` and `run` directives. The `for (;;)` header still needs its `;`.
- `join_continued_lines()` runs in both `Parser` constructors and drops newlines where an
  expression continues: the line ends with a binary operator, `=` or a compound assignment, `,`,
  `.`, `?`, `->`, `=>`, `..`, `..<`, or the next line starts with `.`, `&&`, `||` or `??`.
- `parse_ternary` skips newlines before a `?` or `:` that starts the next line.
- Two statements on one line without `;` report
  `Unexpected token after statement: start a new line or separate with ;`. The wording does not
  start with "Expected ;", so the editor's semicolon filter (`erevos-language/src/checkOutput.ts`)
  does not hide it.
- Tests: `tests/semantics/optional_semicolons.elan`, `tests/semantics/statement_same_line_neg.elan`.
- Docs: "Statements and semicolons" section in `docs/pages/guide/syntax.md`.

## Behavior changes

Code that previously failed with a missing-semicolon error now parses. Code that already parsed
is unchanged: every newline the parser now accepts or drops was a syntax error before.
`examples/lib/entity.elan` and `examples/lib/module.elan` (written without `;`) now parse.

## Verification

- `cmake --build --preset debug` and `--preset release`: success, no new warnings.
- 194 `.elan` files under `tests/` and `examples/` run with the previous release build and the new
  debug build: only the two `examples/lib` files above and the usual time, random and UUID output differ.
- `pwsh tests/run.ps1` (debug and release): every test passes except
  `tests/semantics/no_semicolons.elan`, a file from parallel work. It uses a top-level `const` and a
  `Point { x: 1, y: 2 }` struct literal, and neither parses even with semicolons.
- Discord and Docker plugin tests (release) pass. `python tools/check_builtin_registry.py`: exit 0.

## Not done

- `tests/semantics/no_semicolons.elan` left as is (not from this task).
- Found, not fixed: `.trim()` on a string returns an empty string (also on the previous build).
- Lines starting with `+` or `-` do not continue the previous line, so `-x` can still start a statement.
