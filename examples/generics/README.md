# Generics test suite

Positive cases must exit 0. Negative cases must fail typecheck (non-zero exit) under `@strict`.

## Unsupported / adaptations

- **Generic pointers** (`Box<pointer>`, `Array<ptr>`, etc.): not supported as a first-class generic type argument API. Use runtime `ptr:*` handles outside parametric types. No suite file for this.
- **`make<T>(): T` with no seed**: opaque checking cannot invent a `T` value; positive case uses `make<T>(seed: T)` / `make<int>(0)`. Negative still covers bare `make()` (TC141).
- **`Array<T>[i]` as `T`**: index expr types as unknown; positive uses `useArray<T>` + concrete `nums[0]` print.
- **Method call type args** (`obj.convert<U>(...)`): not parsed; entity test uses free `convert<U>` plus `Container<T>.get()`.
- **Deep nested `>>>`**: write spaces (`> > >`) so lexer/parser keep angle depth.
- **Multi-constraint `&`**: parser no longer eats `&` as a reference qualifier before a type name.

## Run

From repo root (prefer built Debug binary; `erelang` PATH may be stale):

```powershell
.\examples\generics\run.ps1
```

Or one file:

```powershell
.\build\bin\Debug\erelang.exe examples\generics\positive\01_inference_same.elan
.\build\bin\Debug\erelang.exe examples\generics\negative\01_conflicting_inference.elan
```

Smoke test (also covers struct-return field binding):

```powershell
.\build\bin\Debug\erelang.exe examples\generics.elan
```

## Positive

| File | Covers |
|------|--------|
| `positive/01_inference_same.elan` | Same type param twice |
| `positive/02_explicit_multi.elan` | Explicit multi type args |
| `positive/03_nested.elan` | Nested `Map`/`Array`/`Option`/`Pair` |
| `positive/04_aliases.elan` | `StringMap`, `IntList`, `MaybeList` |
| `positive/05_struct_method.elan` | `Box<T>.get()` |
| `positive/06_entity_methods.elan` | `Container<T>.get()` + free `convert<U>` |
| `positive/07_enum_payloads.elan` | Nested enum payloads |
| `positive/08_match_typing.elan` | Match payload as `int` |
| `positive/09_multi_constraints.elan` | `Comparable & Hashable` |
| `positive/10_trait_resolution.elan` | `compareTwice` |
| `positive/11_return_only_param.elan` | Explicit type args (`make<int>(0)`; seed needed under opaque T) |
| `positive/12_nested_subst.elan` | `Outer<T>` / `Wrapper<T>` field access |
| `positive/13_array_first.elan` | `useArray<T>(Array<T>)` + index print |
| `positive/14_kitchen_sink.elan` | `Result<Option<Array<Pair<...>>>>` pipeline |

## Negative

| File | Expect |
|------|--------|
| `negative/01_conflicting_inference.elan` | Conflicting inference |
| `negative/02_explicit_mismatch.elan` | Explicit type mismatch |
| `negative/03_match_wrong_type.elan` | Match payload wrong type |
| `negative/04_partial_constraints.elan` | Only Comparable, missing Hashable |
| `negative/05_no_constraints.elan` | Satisfies neither |
| `negative/06_return_only_infer.elan` | `make()` without type args (TC141) |
| `negative/07_wrong_arity.elan` | `Pair<int>` arity |
| `negative/08_max_unconstrained.elan` | `max(1, "hello")` |
| `negative/09_unknown_type.elan` | `NotComparable` constraint fail |
| `negative/10_enum_payload_mismatch.elan` | Wrong enum payload type |
| `negative/11_wrong_type_args.elan` | Wrong type-arg count |
| `negative/12_unknown_type_param.elan` | Unknown type argument |
| `negative/13_missing_method.elan` | Missing trait method |
| `negative/14_return_type_mismatch.elan` | Declared return mismatch |
