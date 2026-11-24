# Logs

One file per work session, named `YYYY-MM-DD-short-topic.md`. Newest at the bottom of the index.

Each log covers:

- **What was wrong**: the problem, with file and line where it lived.
- **What changed**: the fix, with the files touched and short code excerpts.
- **Behavior changes**: anything a script author could notice.
- **Verification**: how it was tested and the numbers.
- **Not done**: known issues left open, so the next session can pick them up.

## Index

| Date | Log | Summary |
|------|-----|---------|
| 2026-10-21 | [2026-10-21-runtime-perf-and-sized-arrays.md](2026-10-21-runtime-perf-and-sized-arrays.md) | Runtime made 4x to 13x faster, `Array<T> a {N}` fixed-size arrays, typed list/map storage, a dozen bug fixes |
| 2026-10-21 | [2026-10-21-discord-plugin-rewrite.md](2026-10-21-discord-plugin-rewrite.md) | Discord plugin rewritten in current Erelang with mock server tests; `.elan` plugin scripts load, global write fix, WS reader thread, `string.replace/split` |
| 2026-10-22 | [2026-10-22-registry-intellisense-modules-rewrite.md](2026-10-22-registry-intellisense-modules-rewrite.md) | Builtin registry, manifest-driven IntelliSense, modular runtime, process handles, Docker plugin, `examples/types` |
| 2026-10-21 | [2026-10-21-ai-library.md](2026-10-21-ai-library.md) | New `builtin/ai` (infer, generate, explain, optimize, extract, redact), `re.expect`, brace literal and list string fixes |
| 2026-11-23 | [2026-11-23-root-cleanup.md](2026-11-23-root-cleanup.md) | Root cleanup, `build/debug` and `build/release` presets, benchmarks in `examples/bench`, `CLAUDE.md` split into skills |
| 2026-11-23 | [2026-11-23-editor-semantics.md](2026-11-23-editor-semantics.md) | Unsaved compiler diagnostics, scoped editor analysis, core semantics contracts and conformance tests |
| 2026-11-23 | [2026-11-23-linux-macos-port.md](2026-11-23-linux-macos-port.md) | Linux and macOS support: libcurl/OpenSSL/POSIX backends, `unix-*` presets, `sys.os()`, CI on ubuntu and macos |
| 2026-11-24 | [2026-11-24-stray-build-cleanup.md](2026-11-24-stray-build-cleanup.md) | Removed stray `build-editor-analysis/` and generated docs/temp output |
