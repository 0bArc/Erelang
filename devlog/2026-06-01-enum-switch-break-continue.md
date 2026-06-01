# 2026-06-01 - Enum/Switch/Break/Continue Work

## What Was Implemented

- Added `continue` as a first-class statement in the language AST.
- Added parser support for `continue`.
- Added runtime execution support for `continue`.
- Tightened loop control-flow handling so `break` and `continue` are handled consistently in:
  - `while`
  - `for`
  - `repeat`
  - `do while`
  - `for-in`
- Updated `switch` runtime behavior so `break` inside a `switch` does not leak into outer loops.
- Reworked `parse_switch()` to parse `case ...:` / `default:` blocks as regular statement regions (not requiring brace blocks per case).
- Seeded enum members into runtime scope as both:
  - `EnumName::Member`
  - `EnumName.Member`

## Files Changed

- `include/erelang/parser.hpp`
- `include/erelang/runtime.hpp`
- `src/parser.cpp`
- `src/runtime_actions.cpp`
- `src/runtime.cpp`

## Validation

- Rebuilt core targets successfully (`obc`, `erelang_runner`).
- Created `examples/enum_switch_loop.elan` as validation script.
- Switch parsing now advances correctly through `case/default` blocks; additional parser strictness around nested loop-control statements remains to be finalized in follow-up.
