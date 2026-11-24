---
name: repo-hygiene
description: Where files belong in the Erelang repo, how to keep the root and working tree clean, temp file handling, and the logs/ rule. Use for any task that adds, moves, renames or deletes files, writes scratch output, or changes a file (log entry).
---

# Repo hygiene

## Where things go

| Kind of file | Location |
| --- | --- |
| C++ sources and headers | `src/`, `src/runtime/`, `src/runtime/builtins/`, `include/erelang/` |
| Experimental C++ | `experimental/` |
| Build output | `build/debug`, `build/release` (CMake presets only) |
| Conformance tests | `tests/<area>/` (`tests/run.ps1` picks up every `*.elan` recursively) |
| Examples | `examples/<topic>/` |
| Benchmarks | `examples/bench/` (`examples/bench/run.ps1`) |
| Plugin code and plugin tests | `plugins/<name>/`, `plugins/<name>/tests/` |
| Developer scripts | `tools/` (CMake helpers in `tools/cmake/`) |
| Documentation pages | `docs/pages/<section>/`, listed in `docs/mkdocs.yml` |
| Work logs | `logs/YYYY-MM-DD-short-topic.md` |
| Editor extension | `erevos-language/` |
| Scratch, dumps, screenshots, one-off scripts | session scratchpad, outside the repo |

Do not put new files in the repository root. If nothing above fits, ask.

## Never commit or leave behind

- Build folders other than `build/debug` and `build/release`. Do not configure `-B build`, `build-rel`, `build-debug` or similar.
- Generated output: `docs/site/`, `*.vsix` (unless a release is requested), `node_modules/`, `out/`, `compile_commands.json`.
- Test output. Tests that write files use a `tests_tmp_*` name in the working directory (ignored by git) and should delete the file at the end.
- One-off scripts (`tmp-*.ps1`, rename or migration scripts). Run them from the scratchpad, or delete them once used.
- `*.old`, `*.bak`, `*.orig`, `*.log`, `*.tmp`, copied executables, archives.
- New per-change VS Code tasks. `.vscode/tasks.json` keeps one task per job (build debug, build release, run file, tests, benchmarks).

## Before deleting

- Look first. List what a delete removes, with sizes.
- Only delete what you created, or generated build output. Files someone made by hand (archives, notes, test programs) are moved or kept, never deleted.
- Tracked files are removed with `git rm` so the change is visible in the diff.
- Untracked folders that might be someone's work go to the Recycle Bin, not `rm -rf`.
- Do not delete a build folder while `erelang.exe` from it is running (the extension's language server may hold it).

## Logs

Every task that changes a file gets a log, following `logs/README.md`:

- One file per work session: `logs/YYYY-MM-DD-short-topic.md`.
- Sections: What was wrong, What changed, Behavior changes, Verification, Not done.
- Add a row to the index table in `logs/README.md`, newest at the bottom.
- Do not rewrite old logs when paths or behavior change later; write a new log.
