# Erelang language (VS Code / Cursor)

Syntax highlighting, snippets, and IntelliSense for `.elan` files.

The extension host keeps one document index per open file (symbols, entity members, imports). Completions read that index. Disk access for `#include` paths uses a cached directory listing.

```bash
npm install
npm test
npm run package
```

Install `erelang_language.vsix` via **Extensions → Install from VSIX**, or press F5 from this folder.

Settings: `erelang.autoSuggestIdentifiers`, `erelang.debugCompletion`.
