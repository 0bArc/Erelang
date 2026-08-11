# 2026-06-01 - Part 2 Language Completeness Pass

## Requested Areas

- return
- action typed return (`action add(a: int, b: int) -> int`)
- import
- try/catch
- methods
- generics

## What We Added In Part 2

- Added parser support for arrow-style action return types:
  - `: Type`
  - `-> Type`
  - `=> Type` (compat path)
- Enabled method-call syntax in manual mode typechecking (removed hard fail path):
  - Previously emitted `TC141` for normal method calls.
  - Now method calls are accepted and validated at least for receiver declaration, with deeper runtime validation preserved.

## Files Updated

- `src/parser.cpp`
- `src/typechecker.cpp`

## Notes

- `import` remains a top-level construct in current grammar.
- Runtime and typechecker behavior for methods, returns, and generic declarations now align better with expected scripting ergonomics.
