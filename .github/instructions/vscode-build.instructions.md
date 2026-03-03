---
applyTo: '**'
---
Provide project context and coding guidelines that AI should follow when generating code, answering questions, or reviewing changes.

## VS Code Build & Extension Rules

- **Always Refresh the VSIX**  
  Whenever the VS Code language tooling (files under `vscode/erelang-vscode/` or `erelang-language/`) changes, ensure the packaged VSIX is rebuilt and reinstalled. Prefer invoking `cmake --build build --target update_extension --config Release` after source updates, or run `tools/build-and-update-extension.ps1 -Force` directly if you are not in a configured build tree.

- **Keep Syntax Highlighting Current**  
  When new language keywords, built-ins, or syntax constructs are introduced, update both TextMate grammars and language configurations. Verify that `syntaxes/erelang.tmLanguage.json`, `language-configuration.json`, and any supplementary snippet/regex files recognize the new features before finalising the change.

- **Update Autocomplete/Completions**  
  Whenever runtime or builtin APIs change, synchronise completion sources (`server/src/server.ts`, generated `out/server.js`, `src/extension.ts`, etc.) so users immediately see the new symbols. Regenerate compiled outputs (`npm run compile`) after modifying the TypeScript sources.

- **Maintain Search & Navigation Accuracy**  
  Ensure server-side search, symbol lookup, and diagnostics remain aligned with the language. Update search algorithms or indexing logic to cover new constructs and add regression tests or manual verification steps when behaviour changes.

- **Guard Against Performance Regressions**  
  Profile or benchmark the extension when introducing heavier analysis paths. Prefer incremental/lazy computation, caching, and cancellation support in the LSP where feasible. Avoid blocking the main thread, and document known hotspots with TODOs or follow-up issues if a complete fix is out of scope.

- **Documentation & Change Log**  
  When the extension gains new capabilities or a VSIX rebuild occurs, update relevant docs/README files and bump version metadata as part of the same change so users can track what changed.
  
- **Always update devlog, with copyable information to discord**

- **Validation Checklist**  
  Before shipping tooling changes, run `npm run compile`, package the VSIX, and smoke-test key scenarios (syntax highlight, completions, go-to definitions/search) inside a fresh VS Code session. Capture issues early and fix them before marking the task complete.