# Conformance suite

Located at `tests/` with runner `tests/run.ps1`. See `tests/README.md`.

Categories: types, generics, traits, structs, enums, patterns, destructuring, operators, closures, modules, async, concurrency, errors, std, integration.

Positive tests exit 0; `*_neg.elan` must exit non-zero (optional `// expect: …` substring on stderr).

This suite is an evidence lock for 0.x stabilization — it does not replace `examples/`.
