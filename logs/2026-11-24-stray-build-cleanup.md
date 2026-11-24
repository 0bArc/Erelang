# 2026-11-24 Stray build cleanup

## What was wrong

- `build-editor-analysis/` (1.25 GB, Debug, `ERELANG_UPDATE_PATH=ON`) was left in the root by the editor analysis session, against the `build/debug` and `build/release` preset rule.
- Generated output was left in the tree: `docs/site/` (MkDocs build), `docs/hooks/__pycache__/` and an empty `compiler/.mini_ir_run.tmp`.

## What changed

- Deleted `build-editor-analysis/`, `docs/site/`, `docs/hooks/__pycache__/` and `compiler/.mini_ir_run.tmp`. None were tracked by git.
- Kept `build/old-files/` (a hand-made release archive and `ws_smoke.cs`), `build/debug`, `build/release`, `erevos-language/node_modules/`, `erevos-language/out/` and `.cache/clangd`.

## Behavior changes

None. User PATH already pointed at `build/debug/bin/Debug`.

## Verification

- `git status --ignored` lists only `.cache/`, `.github/agents/`, `.github/instructions/`, `.vscode/`, `build/`, `erevos-language/node_modules/` and `erevos-language/out/`.
- `Get-Command erelang` resolves to `build/debug/bin/Debug/erelang.exe`.

## Not done

- `build/old-files/` was left for the owner to move or delete.
- `docs/hooks/__pycache__/` is not git-ignored and will come back on the next docs build.
