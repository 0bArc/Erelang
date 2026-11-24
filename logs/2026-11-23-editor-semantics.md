# Editor analysis and core semantics

## Problems

Editor references/rename used spelling matches across scopes and unrelated open files.
Diagnostics ignored unsaved edits. The typechecker kept an if-branch scope alive while checking
the else branch. Logical evaluation eagerly ran both operands.

## Changes

- `src/obs_main.cpp`: `--check-stdin <file>` checks a source overlay without rewriting the root
  file; recursive imports retain the original directory. A tab-delimited `!token` stream supplies
  compiler token locations to tooling. Existing `--check` behavior is unchanged.
- `erevos-language/src/scopeAnalysis.ts`: bounded shared lexical binding index, compiler-token
  ingestion and UTF-8 to UTF-16 conversion, local fallback tokenizer while edits settle.
- `analysis.ts`, `completions.ts`, `server.ts`: scope-aware identifiers, hover, references and
  rename. Collision/capture checks, versioned edits, cancellation, conservative refusal to partially
  rename interpolation or members. Explicit imports drive cross-file definitions; explicit
  reference/rename requests can discover closed files with a 2,000-file / 8 MB bound. No scan on edits.
- `src/typechecker.cpp`: destroy the then-branch scope before checking the else branch.
- `src/runtime/eval.cpp`, `bytecode.cpp`: lazy `&&`, `||`, `??`; the current bytecode path falls back
  to interpretation for these operators rather than evaluating both operands.
- `docs/pages/guide/semantics.md`: contracts for bindings, integer arithmetic, conversions,
  equality, evaluation, ownership and async boundaries, linked to executable evidence.
- Extension version 0.1.1; rebuilt VSIX.

## Validation

66 extension tests, including real LSP unsaved-buffer diagnostics, compiler stdin imports,
UTF-8 locations, scope-safe rename and a rename into a closed importing file.
91 language conformance cases pass, including 13 new semantics cases.
Local diagnostic benchmark: 29,889 characters / 9,000 tokens indexed in about 17 ms;
resolving all tokens took about 2.5 ms. Timing is local evidence, not an editor latency guarantee.

## Remaining limits

Compiler tokens are not a full export of inferred expression types or semantic symbol IDs.
Namespace/member and interpolation rename needs precise expression spans; unsupported renames are
refused. Cross-file navigation currently follows direct quoted script imports. The compiler reads
imported modules from disk, even when another editor buffer has unsaved dependency edits.
Native-backend parity and async scheduling guarantees remain outside this change.

## Copyable update

Erelang 0.1.1 editor update: unsaved compiler diagnostics, shared scope-aware identifier analysis,
safer rename/navigation, and compiler token locations. Added core semantics documentation and
13 conformance cases. Fixed branch-scope leakage and lazy logical evaluation. Validation:
66 extension tests and 91 language cases pass. Full inferred-type export remains future work.
