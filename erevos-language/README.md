# Erelang language (VS Code / Cursor)

Syntax highlighting, snippets, and IntelliSense for `.elan` files.

Version 0.1.2 removes missing-semicolon diagnostics, including expectations forwarded by older
compiler builds. Other syntax and type diagnostics remain enabled.

The extension host keeps one document index per open file (symbols, entity members, imports). Completions read that index. Disk access for `#include` paths uses a cached directory listing.

Compiler diagnostics check unsaved buffers after 800 ms of quiet (1.5 seconds during edit/save bursts).
Checks run one at a time, including file reads. Editing or closing a file cancels its pending/active
check; older versions cannot publish results. Checks stop after 10 seconds or 1 MiB of output.
This requires an updated compiler with `--check-stdin <file>` support. Imports retain the original
file's directory; the extension does not rewrite source files to run checks. Imported modules are
read from disk by the compiler, so save changes to dependencies before relying on their diagnostics.

Rename, references, hover and identifier completions share a bounded lexical binding index. The
compiler supplies tokens after each check; a local tokenizer serves requests while edits settle.
Rename rejects collisions and changes that would leave string interpolation partially renamed.
Go to Definition follows direct quoted script imports. Explicit reference/rename requests discover
closed workspace files asynchronously (up to 2,000 files / 8 MB). Analysis declines larger projects
instead of applying an incomplete rename; edits never trigger workspace scans. Full inferred-type export,
transitive import navigation, namespace/member identity and interpolation spans remain future work.

Bulk edits pause heavy completions in the edited file for 400 ms. Symbol indexing skips files above
8,000 lines, 500,000 characters, or a 20,000-character line to keep generated sources responsive.
Syntax highlighting remains available for those files.

```bash
npm install
npm test
npm run package
```

Install `erelang_language.vsix` via **Extensions → Install from VSIX**, or press F5 from this folder.

Settings: `erelang.autoSuggestIdentifiers`, `erelang.debugCompletion`.
