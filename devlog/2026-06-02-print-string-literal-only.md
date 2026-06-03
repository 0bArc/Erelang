# 2026-06-02 — Print expression + VSIX highlighting (part 1)

## Correction (same day)

An earlier pass required `print` to use only string literals (`TC080` / parser check). That broke valid programs such as `print 1` and `print 1 + 4 + 5` in `examples/program.elan`.

**Reverted:** parser and typechecker again accept any expression after `print` (numbers, arithmetic, calls, quoted strings with `{interpolation}`).

## VSIX fix

Bare `print` args looked uncolored (white) after removing the old `printArgs` rule, which only highlighted a single identifier.

**Added** `#printStmt` in `erevos-language/syntaxes/erevos.tmLanguage.json`: a region from `print` through `;` that re-includes strings, numbers, calls, member access, operators, and identifiers so `print 1`, `print add(2, 3)`, and `print 1 + 4 + 5` keep normal token colors.

## Still valid

- `print "hello"`
- `print "{name}"` / `print "{add(2, 3)}"` (interpolation in strings)

## Verify

```bash
cmake --build build --target erelang_runner
erelang examples/program.elan
```

Reload VS Code window after grammar change (or reinstall extension) to pick up highlighting.

See [2026-06-02-return-types-and-control-flow.md](2026-06-02-return-types-and-control-flow.md) for part 2 (return types, bool control flow, visibility).
