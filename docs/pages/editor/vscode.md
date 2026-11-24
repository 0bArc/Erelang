# VS Code extension

The Erelang extension adds highlighting, snippets, diagnostics from the typechecker, and a
debugger for `.elan` files. It ships as a `.vsix` package.

[:material-download: Download erelang-language.vsix](../assets/erelang-language.vsix){ .md-button .md-button--primary }

## Install

=== "VS Code UI"

    1. Open the Extensions view (`Ctrl+Shift+X`).
    2. Open the `...` menu and choose **Install from VSIX...**.
    3. Pick `erelang-language.vsix`.

=== "Command line"

    ```bash
    code --install-extension erelang-language.vsix
    ```

Reload the window afterwards. Files ending in `.elan` use the Erelang language mode.

## What you get

<div class="grid cards" markdown>

- :material-palette: **Highlighting**

    TextMate grammar and semantic tokens for actions, types, builtins and interpolation.

- :material-bug-check: **Diagnostics**

    `TC` codes from the typechecker, shown as you type. See [Diagnostics](../guide/diagnostics.md).

- :material-code-braces: **Snippets**

    Actions, entities, loops and `#include` lines.

- :material-play-circle: **Run and debug**

    Run a script or attach the debugger from the editor.

</div>

## Highlighting in these docs

Code blocks tagged `elan` use a lexer built from the extension grammar, so what you read here
matches what you see in the editor:

```elan
@erelang
#include <builtin/network> as net

public action greet(name: string): string {
    return "Hello, {name}!";
}

public action main() {
    Array<int> scores {3} = [10, 20, 30];
    print greet("Erelang");
}
run main;
```

## Building the package yourself

The sources are in `erevos-language/`. From that folder:

```bash
npm install
npx vsce package
```
