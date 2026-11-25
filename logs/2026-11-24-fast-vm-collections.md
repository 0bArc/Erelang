# 2026-11-24 Fast path: mixed numbers, conversions, lists and maps

## What was wrong

The bytecode fast path (`src/runtime/fast_vm.cpp`, see `2026-11-24-fast-vm.md`) left three
common cases on slow paths:

- Mixed `int` / `float` arithmetic and float comparisons went through the generic
  `apply_binary` for every operation; only `int op int` was inlined.
- A typed `let` (`float y = ...`) built the runtime type name as a string on every execution
  to check it.
- `int()` / `float()` / `string()` / `bool()` did not compile at all; on the tree-walker each
  call formats its argument and walks the builtin dispatcher (about 800 ns).
- Any list or map made the whole loop or action fall back to the tree-walker, because values
  read from a collection may be heap, weak or file handles or struct markers, which the
  tree-walker releases or expands when they are bound to a name.

## What changed

- `fast_vm.cpp`
  - Int/float and float/float `+ - * / % **` and comparisons are inlined in `Bin` and the
    fused compare-and-jump, with the same results and error messages as `apply_binary`.
    Unary minus is inlined.
  - `CheckType` has a per-declaration bit mask of the value kinds that always match, computed
    at compile time from `runtime_declared_type_matches`; only other values take the old path.
  - `int()`, `float()`, `string()`, `bool()` compile once no action, struct or variable has
    that name. `int` / `float` of an int or float run natively and reproduce the dispatcher's
    format-and-reparse exactly (`float(500000)` is still the string `"5e+05"`); other
    arguments use the dispatcher.
  - New ops for list and map literals, ranges, `xs[i]`, `xs[i] = v`, list and map methods
    (expression and statement form, the statement form writes `_` as the tree-walker does),
    and `for (x : xs)` / `for (k, v : m)` over lists and maps (map loops iterate a copy, as the
    tree-walker does). `ForInStmt` is a new loop entry point (`Runtime::fast_loop`).
  - A method call compiles only when its receiver is a variable whose every write is a typed
    `let` of a list or map type, or a parameter / outer variable checked on entry
    (`FastCode::entryKinds`). The method must be one `collection_method` handles for that kind.
  - Collection safety: code that touches collection contents is "aware". Every list and map
    that enters aware code from the tree-walker is scanned (`contents_safe`, cycle-safe); a
    heap, weak, file or other non-collection handle, or a struct marker, sends the call or
    loop back to the tree-walker. Aware code may only call compiled, "slow-free" code (no
    tree-walker calls anywhere below it), so nothing can store such a value while it runs.
    Programs whose structs define `add` / `sub` / `mul` / `div` / `eq` do not compile aware
    code, since the tree-walker may run those methods for `a op b` on any non-number.
  - Scanning is capped by work: all scans together may visit 4M elements plus 8 per
    collection operation executed; past that, entries needing a scan use the tree-walker.
- `eval.cpp`, `exec_decl.cpp`, `include/erelang/runtime.hpp`: list/map indexing, index
  assignment, list / range / map creation and the list of known collection methods moved into
  shared functions (`collection_index_get`, `collection_index_set`, `make_list_value`,
  `make_range_list`, `new_dict_value`, `collection_method_known`) used by the tree-walker and
  the fast path. Behavior unchanged.
- `exec_stmt.cpp`: `exec_for_in` tries `fast_loop` first.
- Tests: `tests/semantics/fast_collections.elan`, `fast_list_bounds_neg.elan`,
  `fast_fixed_push_neg.elan`.
- Docs: `docs/pages/examples/bench.md` (what runs fast, sample numbers).

## Behavior changes

None intended. Only speed.

## Verification

Release build, best of 2, from scripts in the session scratchpad (not committed):

| Workload | Before | After |
|----------|--------|-------|
| `acc = acc + i * 0.5` loop 2e6 | 89 ms | 35 ms |
| `float y = i * 1.5` typed let 1e6 | 238 ms | 17 ms |
| `float(i) / 3.0` + `int(x)` 1e6 | 1622 ms | 41 ms |
| float action call 1e6 | 81 ms | 65 ms |
| list push 1e6 | 211 ms | 71 ms |
| `xs[i]` sum with `xs.len()` 1e6 | 314 ms | 43 ms |
| for-each sum 1e6 | 71 ms | 10 ms |
| map set/get 2e5 | 170 ms | 98 ms |
| sieve to 2e6 | 1320 ms | 242 ms |
| 2e5 binary searches in a 1e6 list | 2082 ms | 196 ms |
| 300x300 nested list build and sum | 31 ms | 8 ms |

`examples/bench/run.ps1` (work ms, best of 3): dict 29 to 8, list 35 to 6, others unchanged.

- `cmake --build --preset release` and `--preset debug` (`erelang_runner`): success, no new
  warnings in the changed files.
- `pwsh tests/run.ps1` (debug): pass=100 fail=1. The failure is
  `tests/semantics/no_semicolons.elan`, which fails the same way on the build before this
  change (uncommitted optional-semicolon work in the tree).
- Every `.elan` under `tests/` and `examples/` (203 files) run with the release build from
  before this change and the new one, comparing exit code, stdout and stderr: 4 differ, all
  random or time output (`examples/random/crypto_sha256.elan`, `examples/types/bench.elan`,
  `tests/std/random.elan`, `tests/std/time.elan`).
- New tests pass on both the old and the new build (same output); the two negative tests give
  the same error on both.
- Probes compared on both builds: handle ids of nested map literals, list and map
  interpolation, `join` / `contains` / `find` / `getOr` / `remove` / `pop` on empty lists, map
  key conversion in `for (k, v : m)`, and a heap value inside a list (released the same way,
  `mem_alive()` 0).
- `compiler/main.elan` (self-host): identical output, 321 ms on both (struct-heavy, no change).
- Discord and Docker plugin tests (release): all pass.

## Not done

- Map keys that read like handles (`"file:3"`) are converted to handles by `keys()` and
  `for (k, v : m)` on both paths; inside compiled code such a value is not released at the
  end of an action as the tree-walker would. Same class as the existing case of string
  operations producing handle-like text.
- Sets, `forEach`, struct values in collections, and method calls on loop variables
  (`for (row : grid) { row.len() }`) still run on the tree-walker.
- Mutual recursion between actions that read lists does not compile (each needs the other to
  be compiled first).
- Linux and macOS were not built here; CI covers them.
