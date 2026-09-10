# Erelang

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&amp;logo=cplusplus&amp;logoColor=white" />
  <img alt="CMake" src="https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&amp;logo=cmake&amp;logoColor=white" />
  <img alt="Windows" src="https://img.shields.io/badge/platform-Windows-0078D4?style=flat-square&amp;logo=windows&amp;logoColor=white" />
  <img alt="Version" src="https://img.shields.io/badge/version-0.0.1-7c3aed?style=flat-square" />
  <img alt="License" src="https://img.shields.io/github/license/0bArc/Erelang?style=flat-square" />
  <img alt="Last commit" src="https://img.shields.io/github/last-commit/0bArc/Erelang?style=flat-square" />
</p>

Scripting language and C++20 interpreter for `.elan` programs. Windows-first.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target erelang_runner -j 8
./build/bin/Debug/erelang.exe examples/program.elan
```

Optional: `-DERELANG_EXPERIMENTAL=ON` for `builtin/threads` and `builtin/monitor`.

<pre style="background:#111111;color:#cdd6f4;padding:16px 18px;border:1px solid #222;border-radius:8px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:13px;line-height:1.55;overflow-x:auto"><span style="color:#89b4fa">@erelang</span>
<span style="color:#6c7086">#include</span> <span style="color:#a6e3a1">&lt;builtin/math&gt;</span> <span style="color:#cba6f7">as</span> math

<span style="color:#cba6f7">public</span> <span style="color:#89b4fa">action</span> <span style="color:#89dceb">main</span> {
    <span style="color:#89b4fa">print</span> math.<span style="color:#89dceb">add</span>(<span style="color:#fab387">2</span>, <span style="color:#fab387">3</span>);
}

<span style="color:#cba6f7">run</span> main;
</pre>

Modules are import-gated (`#include <builtin/fs> as fs`). Language, builtins, and CLI: [docs/](docs/README.md). Editor support: `erevos-language/`.

Apache-2.0. See [LICENSE](LICENSE).
