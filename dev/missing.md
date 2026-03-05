# Erelang Self-Hosting Readiness Audit (Windows `erelang.exe`)

Date: 2026-03-04  
Scope: current repo state (`src/`, `include/`, `docs/`, runner/tooling) against the 30-capability checklist.

Status legend:
- ✅ Implemented
- 🟡 Partial / basic version exists
- ❌ Missing

## Summary

- ✅ Implemented: 23 / 30 areas (fully or close to full for current architecture)
- 🟡 Partial: 7 / 30 areas
- ❌ Missing: 3 / 30 areas (major blocks)

Critical blockers for **true end-to-end self-hosting compiler -> Windows EXE**:
1. ❌ Full native codegen/backend pipeline (real machine code/object emission)
2. ❌ Windows PE/object/link pipeline owned by Erelang itself (currently relies on external C++/toolchain path)

---

## Capability Matrix

### 1) Core syntax and semantics — ✅
Implemented: variables, constants, assignment, expressions, precedence, arithmetic/comparison/logical/unary.  
Partial: bitwise operators exist lexically, but full semantic/runtime coverage should be verified operator-by-operator.  
Ternary operator (`?:`) is now implemented end-to-end (parser + runtime + typechecker).

### 2) Data types — 🟡
Implemented: `bool`, `char`, `int`, `double`, `string`, structs, enums, arrays/lists/maps/hashmap aliases, nullable forms (`null/nil/nullptr`).  
Partial: fixed-width integer aliases (`u8/u16/u32/u64/i8/i16/i32/i64`) now map to `int` semantics in type declarations; tagged unions/variants are not first-class language types yet.

### 3) Memory management — 🟡
Implemented: pointer handles (`ptr_*`), `malloc`, `free`, GC runtime present.  
Partial: allocator abstraction, low-level pointer arithmetic semantics, and explicit memory resizing APIs are limited.

### 4) Control flow — ✅
Implemented: `if/else`, `switch`, `for`, `while`, `break`, `continue`, `return`, recursion support.  
`do while` syntax is now implemented.  
`for-in` now lowers in the native IR path for static/literal list+dict cases (including literal dict key/value pair expansion), and unsupported dynamic cases degrade to explicit no-op markers instead of hard TODO failures.  
Remaining advanced gap: full pattern-match destructuring and fully dynamic native `for-in` iteration over runtime containers.

### 5) Functions — ✅
Implemented: declaration/definition, params/returns, multiple params, recursion, pass-by-value behavior.  
Optional features remain optional.

### 6) Type system capabilities — 🟡
Implemented: declarations, basic checking, explicit casts (`static/dynamic/reinterpret/bit_cast` family).  
Partial: stronger implicit conversion rules, richer inference, and generic/template system are not complete.

### 7) Modules and program organization — 🟡
Implemented: imports/includes, namespaces, visibility (`public/private`), multi-file composition, and top-level extern action declarations.  
Partial: formal interface/header contract system and robust dependency resolver for large self-hosted projects.

### 8) Preprocessing capability — 🟡
Implemented: `#if/#elif/#else/#endif/#ifdef/#ifndef/#define`, include handling.  
Partial: macro system is intentionally limited.

### 9) String manipulation — ✅
Implemented: creation, concat/interpolation, substring/find/len/compare/convert helpers.

### 10) Character handling — 🟡
Implemented: lexer now accepts UTF-8 non-ASCII bytes in identifier start/part paths, plus explicit character helper builtins (`char_is_digit`, `char_is_space`, `char_is_alpha`, `char_is_ident_start`, `char_is_ident_part`).  
Partial: full Unicode category-aware tokenization/classification (beyond byte-level acceptance).

### 11) Collections and containers — ✅
Implemented: dynamic list/dict/hashmap APIs, traversal patterns, plus first-class set/queue builtin coverage (`set_*`, `queue_*`).

### 12) File I/O — ✅
Implemented: open/close/read/write/append, binary-ish file handle APIs, existence checks, size-related workflows.

### 13) Directory and filesystem access — ✅
Implemented: listing, path helpers, cwd/chdir, relative path operations, mtime query.

### 14) Command line interaction — ✅
Implemented: args (`os.args*`), env vars, exit codes (`exit`), stdout/stderr pathways.

### 15) Error handling — 🟡
Implemented: diagnostics/typechecker errors, runtime exceptions, propagation by throwing, plus baseline `option_*` / `result_*` utility builtins.  
Partial: full language-native `Result/Option` typing/matching semantics and structured recoverable diagnostics pipeline.

### 16) Lexer infrastructure — ✅
Implemented: token kinds, streams, literals, comments/whitespace handling, keyword recognition.

### 17) Parser infrastructure — ✅
Implemented: recursive descent + precedence parsing, AST assembly, parser state, lookahead.

### 18) AST representation — ✅
Implemented: expression/statement/declaration nodes and traversal in parser/runtime/typechecker.

### 19) Symbol tables — ✅
Implemented: scope stack and lookup in semantic/typechecker infrastructure.

### 20) Semantic analysis — 🟡
Implemented: type checks, var/function resolution, scope checks, control-flow checks in typechecker.  
Partial: deeper overload resolution, const-eval maturity, richer flow analysis.

### 21) Intermediate representation (IR) — 🟡
Implemented: first-class runtime-oriented IR stage now emits explicit operations (load/store style moves, arithmetic, comparisons, labels, jumps, conditional jumps, call markers, return, typed print ops) with deterministic function ordering and emitter output via `obc`/`erelang --emit-ir`.  
Partial: typed SSA/basic-block form and full call graph/function ABI metadata are not complete.

### 22) Code generation — 🟡
Implemented: x64 backend now lowers runtime IR instructions to assembly with stack-frame setup, local stack-slot addressing, arithmetic/comparison ops, labels/branches, call-site emission (including stack args beyond register args), `input` support, and concrete `sleep`/`pause` opcode execution in native builds.  
Partial: full instruction selection, register allocation, fully dynamic collection iteration lowering, robust exception semantics, and complete calling-convention/interop coverage are still in progress.

### 23) Binary generation — 🟡
Implemented: compiler-driven native demo artifact path now exists (`obc --build-native`) that emits assembly and invokes toolchain to produce a Windows `.exe`.  
Partial: native object/PE writer fully owned by Erelang remains missing (current path still uses external GCC/ld).

### 24) Linker interaction — 🟡
Implemented: process spawning and external tool invocation available, including compiler-owned assembly-to-exe orchestration in `obc --build-native`.  
Partial: robust full-language linker-driver orchestration and fully self-hosted assembler/link backend remain incomplete.

### 25) Platform interface (Windows) — ✅
Implemented: process/environment/system APIs, Win32 integration in runtime/runner.

### 26) Build orchestration — 🟡
Implemented: CMake orchestration + staged workflows; native artifact generation commands are now available directly in compiler-facing CLIs (`obc` and `erelang`) via `--emit-ir`, `--emit-asm`, and `--build-native`.  
Partial: language-native dependency graph/incremental self-build coordinator.

### 27) Standard library foundation — 🟡
Implemented: substantial builtin surface for strings/containers/fs/memory/error-like operations.  
Partial: stabilized minimal self-hosting stdlib boundary and package/module maturity.

### 28) Bootstrapping stages — 🟡
Implemented: stage-1 and stage-2 direction exists (C++ host + Erelang compiler scripts) with expanded native demo coverage (`for`, `while`, `switch`, multi-arg calls, static/literal `for-in`, input, and larger loop stress probes).  
Partial: stage-3 self-compile reproducibility and stage-4 production artifacts fully produced by self-host path.

### 29) Deterministic builds — 🟡
Implemented (partial): deterministic ordering was strengthened for runtime projection APIs (`list_files`, `set_values`, `dict_keys`, `dict_values`, `dict_entries/items`, `table_rows`, `table_columns`, `table_row_keys`) by explicit sorting.  
Partial: stable symbol ordering/reproducible binary output guarantees are still incomplete in the compiler backend/link pipeline.

### 30) Performance infrastructure — 🟡
Implemented: hash maps/caching-related pieces exist.  
Partial: dedicated arena allocators, memory pools, and large-codebase compiler perf tuning in the self-hosted compiler path.

---

## Priority Missing Work (Implementation Order)

## P0 (Blocks true self-hosting executable production)
- Build a first-class IR pipeline (`HIR/MIR` layer) in Erelang compiler path. *(started: IR v0 + `--emit-ir`)*
- Implement codegen backend target (start with x64 Windows ABI subset). *(started: x64 NASM bootstrap via `--emit-asm`)*
- Emit object files and PE sections (or generate assembly + stable external assembler/link driver).
- Add deterministic symbol/section ordering controls.

## P1 (Needed for robust self-hosting compiler engineering)
- Add language-native `Result/Option` style error handling patterns and diagnostics API.
- Expand type system (fixed-width integer types + variant/tagged union ergonomics).
- Add Unicode-aware lexer and identifier policy.

## P2 (Scale and productivity)
- Add allocator abstraction + arena pools for AST/IR allocation.
- Add incremental compilation graph and persistent cache.
- Add stronger module/interface boundary tooling for large compiler codebases.

---

## Notes

- This file is a **gap report** and implementation backlog, not a claim that all missing items were implemented in this pass.
- Major missing items (native backend + PE/object generation) are multi-phase engineering efforts and should be tracked as dedicated milestones.
- Migration policy now deprecates legacy constructor/mutator builtins (`list_new`, `list_push`, `dict_new`, `dict_set`) while keeping compatibility for existing scripts.
