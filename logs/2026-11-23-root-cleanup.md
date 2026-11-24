# 2026-11-23 Root cleanup and build layout

## What was wrong

- The repository root held generated test output, stale build folders and an old editor extension.
- Builds lived in sibling folders (`build/`, `build-rel/`, `build-debug/`, `build-exp/`) configured by hand.
- `CMakeLists.txt` copied every `erelang.exe` into `<build>/bin/Debug`, `<build>/bin` and `%LOCALAPPDATA%/Erelang/bin`, so a release build overwrote the "Debug" folder and both builds fought over PATH.
- Benchmarks sat in a top-level `bench/` folder while `examples/bench/` existed and was empty.
- `.vscode/tasks.json` held about 20 one-off "Rebuild after ..." tasks pointing at a missing `build/erelang.exe`.
- `CLAUDE.md` kept every rule in one long file and had no rule about file placement.

## What changed

Cleanup:

- `tests_tmp_compiler.txt`, `tests_tmp_log.txt`: removed and untracked. `tests/std/*.elan` write `tests_tmp_*` files to the working directory, so `.gitignore` now ignores `tests_tmp_*`.
- `tools/tmp-rebrand-erelang.ps1`: removed (one-off rename script, already applied).
- `build-debug/` (563 MB) and `build-exp/` (26 MB): deleted.
- `vscode/` (111 MB, untracked 0bskript extensions, superseded by `erevos-language/`): moved to the Recycle Bin.
- Old `.vscode/tasks.json`: moved to the Recycle Bin and replaced by five tasks (build debug, build release, run active file, run tests, run benchmarks).

Build layout:

- New `CMakePresets.json`: `debug` builds into `build/debug`, `release` into `build/release` (MinGW Makefiles).
- `CMakeLists.txt`: removed the three copy commands. New option `ERELANG_UPDATE_PATH` (ON; OFF in the release preset) runs `tools/cmake/ensure-erelang-path.cmake` with the runner's own output folder.
- `tools/cmake/ensure-erelang-path.cmake`: drops every other PATH entry inside the source tree instead of matching `build/bin/Release`.
- `src/obs_main.cpp` `detect_build_layout`: project root is the first parent of the build folder holding `CMakeLists.txt`, so nested `build/debug` still resolves to the repo.
- Old `build/` (Debug) and `build-rel/` replaced by the preset builds. `build/bin/Debug/Erelang Release.rar` and `build/ws_smoke.cs` were not generated and were kept in `build/old-files/`.
- `bench/` moved to `examples/bench/`; `run.ps1` resolves paths from its own folder.
- Paths updated in `README.md`, `CONTRIBUTING.md`, `docs/pages/guide/getting-started.md`, `docs/pages/guide/selfhost.md`, `docs/pages/examples/bench.md`, `tests/run.ps1`, `tests/README.md`, `examples/generics/*`, `plugins/*/README.md`, `tools/*.ps1`, `tools/check_builtin_registry.py`, `erevos-language/src/erelangPath.ts`, `erevos-language/package.json`, `erevos-language/test/completions.test.js`, `.vscode/launch.json`, `.cursor/rules/erelang-sync-all.mdc`, `.github/instructions/vscode-build.instructions.md`.
- `tools/fix-erelang-path.ps1`: rewritten to put `build/debug/bin/Debug` first and drop other repo entries; no longer copies executables.

Instructions:

- `CLAUDE.md` trimmed to project, skills table, repository map, clean-files rules, workflow, completion criteria and report format.
- New skills in `.claude/skills/`: `repo-hygiene` (file placement, temp files, deletes, logs), `build-and-test` (presets, paths, tests, extension, release rules), `language-feature` (end-to-end feature and builtin checklist, docs), `cpp-quality` (C++ style and runtime correctness).
- `.gitignore`: `tests_tmp_*`, `.claude/settings.local.json`.

## Behavior changes

- Interpreter paths: `build/debug/bin/Debug/erelang.exe` and `build/release/bin/Release/erelang.exe`.
- Only the debug build updates the user PATH. `%LOCALAPPDATA%/Erelang/bin` is no longer refreshed; an old copy there may be stale.
- The extension's automatic exe search only checks the two new paths, then PATH.

## Verification

- `cmake --preset debug` + `cmake --build --preset debug -j 12`: success; post-build printed `PATH: ...\build\debug\bin\Debug first`.
- `cmake --preset release` + `cmake --build --preset release -j 12`: success. No new warnings (existing ones in `src/lexer.cpp`, `experimental/builtins/*.cpp`).
- `examples/program.elan` runs on both executables.
- `tests\run.ps1` (debug build): pass=78 fail=0 skip=0.
- `pwsh -File examples\bench\run.ps1 -Runs 1`: all five benchmarks ran.
- `python tools\check_builtin_registry.py` (release build): exit 0.
- `erevos-language`: `node --test test/completions.test.js` 14/14 pass (manifest test found the new release path).

## Not done

- `dev/missing.md` is tracked although `dev/` is ignored; left in place.
- `tests/std/log_basic.elan` and others leave their `tests_tmp_*` file behind instead of deleting it.
- `examples/bench/run.ps1` fails under Windows PowerShell 5.1: `Set-Content -Encoding UTF8` writes a BOM into the empty startup script. Works under `pwsh` 7; the VS Code tasks use `pwsh`.
- `build-editor-analysis/` (Debug, `ERELANG_UPDATE_PATH=ON`) was configured by another session during this work and left in place. It should move to the `debug` preset, or set `ERELANG_UPDATE_PATH=OFF` so it does not take over PATH.
- Old logs still mention `build-rel` and `build/bin/Debug`; they are history and were not rewritten.
