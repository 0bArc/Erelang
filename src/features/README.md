# Feature Modules

This folder is the extension point for adding runtime language features without bloating `src/runtime.cpp`.

## Pattern

1. Add a focused header in `include/erelang/features/`.
2. Add implementation in `src/features/`.
3. Register the new source file in `CMakeLists.txt` (`erelang` target).
4. Wire the feature into runtime/typechecker/VSIX as needed.

## Implemented modules

- `serialization.cpp`
  - `to_json(...)`
  - `from_json(...)`
  - JSON string escaping helpers

## Template for new feature

- Add `include/erelang/features/my_feature.hpp`
- Add `src/features/my_feature.cpp`
- Keep API small and pure where possible (`std::string` in/out)
- Integrate via `Runtime::eval_builtin_call` and matching typechecker metadata
