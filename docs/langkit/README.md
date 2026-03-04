# LangKit Bootstrap (Erelang)

This is a starter path for building a new language **using Erelang** as the host runtime.

## What is included now

- Core host features needed for language work:
  - `struct` runtime field storage
  - `Map<string, any>` / `Array<T>`
  - `to_json(...)` / `from_json(...)`
- Extension point folder:
  - `src/features/` and `include/erelang/features/`

## Recommended architecture

1. **Tokenizer pass** in Erelang
   - Input source string
   - Output token list (`Array<string>` or `Array<Map<string, any>>`)
2. **Parser pass** in Erelang
   - Consume token list
   - Build AST as `Map<string, any>` nodes
3. **Evaluator / compiler pass**
   - Walk AST and execute in Erelang or emit target code
4. **Feature modules in C++**
   - Add hot-path builtins in `src/features/` when Erelang scripts need speed

## Minimal host contract

- Use maps for nodes:
  - `{ "kind": "Binary", "op": "+", "left": ..., "right": ... }`
- Use arrays for ordered lists:
  - params, statements, token stream
- Persist snapshots with JSON helpers for debugging.

## Next implementation target

- Add builtins:
  - `langkit_tokenize(source)`
  - `langkit_parse_expr(tokens)`
  - `langkit_eval(ast)`

This keeps Erelang as the meta-language host while incrementally moving heavy steps into `src/features/`.
