# Erelang Language (erelang)

VS Code syntax highlighting and language basics for `.0bs` files.

Features:
- TextMate grammar for keywords, directives, actions, entities
- Language config for comments, brackets, and auto-closing
- Snippets for actions and entities
- Missing-semicolon diagnostics across statements
- Context-aware completions for keywords (`run`, loops), symbols, and collection methods (`Array<T>`, `Map<K, V>`)
- Reduced legacy collection completion noise (deprecated `list_*` / `dict_*` entries hidden)
- Identifier autosuggest retriggers while typing or deleting names, but does not reopen suggestions on plain Enter/newline edits

Install (local dev):
1. `npm install`
2. `npm run watch`
3. Press F5 to launch Extension Development Host

Settings:
- `erelang.autoSuggestIdentifiers`: keep identifier autosuggest responsive while typing/deleting without forcing popup reopen on Enter.
- `erelang.debugCompletion`: emit completion context traces to the `Erelang Language Debug` output channel.

