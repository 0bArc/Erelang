# Erelang Language (erelang)

VS Code syntax highlighting and language basics for `.0bs` files.

Features:
- TextMate grammar for keywords, directives, actions, entities
- Language config for comments, brackets, and auto-closing
- Snippets for actions and entities
- Missing-semicolon diagnostics across statements
- Context-aware completions for keywords (`run`, loops), symbols, and collection methods (`Array<T>`, `Map<K, V>`)
- Reduced legacy collection completion noise (deprecated `list_*` / `dict_*` entries hidden)

Install (local dev):
1. `npm install`
2. `npm run watch`
3. Press F5 to launch Extension Development Host

