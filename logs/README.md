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
