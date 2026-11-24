# Conformance suite

Located at `tests/` with runner `tests/run.ps1`. See `tests/README.md`.

Categories: types, generics, traits, structs, enums, patterns, destructuring, operators, closures, modules, async, concurrency, errors, std, integration.

Positive tests exit 0; `*_neg.elan` must exit non-zero (optional `// expect: …` substring on stderr).

This suite is an evidence lock for 0.x stabilization; it does not replace `examples/`.

The [core semantics](semantics.md) chapter maps language rules to executable tests.
`tests/semantics/` locks down numeric boundaries, lexical scope, const rebinding, conditions,
equality, and lazy logical evaluation. Positive tests use assertions (`fail` on a mismatch),
so an exit code of zero verifies the result rather than merely accepting successful parsing.
