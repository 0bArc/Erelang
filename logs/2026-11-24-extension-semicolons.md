# Extension 0.1.2: remove semicolon expectations

The extension's heuristic validator emitted `Missing semicolon (;)`, and older compiler checks could
forward `Expected ;` messages into the Problems panel even when the user wanted newline-separated code.

Removed the semicolon heuristic and its unused compatibility entry points from `diagnostics.ts`.
`checkOutput.ts` suppresses semicolon-expectation parser messages and empty syntax-error batch headers,
while keeping other syntax, loader and type errors. The compiler grammar is unchanged by this update.

Validation: all 69 extension tests passed using the release executable (the debug executable was
being linked by another build). Regression cases cover semicolon-free statements, old compiler
message formats, mixed error batches and preserving unrelated errors mentioning `;`.

Rebuilt `erevos-language/erelang_language.vsix` as version 0.1.2 and refreshed the documentation
download. Deletion of the obsolete 0.0.29 and 0.0.34 VSIX packages was attempted with explicit
PowerShell literal paths, but automatic approval review rejected it as blocked by policy. Both remain.

Copyable update: Erelang extension 0.1.2 removes missing-semicolon expectations. Other diagnostics
stay active. Rebuilt VSIX and refreshed docs download. Old-package deletion was blocked by the
execution policy. 69 tests pass.
