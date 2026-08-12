# Erelang Runtime Review

## Audit Summary

Date: 2026-08-11
Scope: Full runtime audit across src/runtime/, src/parser.cpp, src/lexer.cpp, src/optimizer.cpp
Severity: P0=crashes/security, P1=incorrect behavior, P2=minor issues

---

## P0 — Crash / Security / UB

### 1. system.cpp — Process Execution
**Status: SAFE**  
- Uses `CreateProcessW` (Windows) directly — no shell injection.  
- Each argument is individually quoted via `quote_windows_arg` (double backslashes, wrap in quotes).  
- Pipe read has a 60-second timeout to prevent indefinite hangs.  
- No `std::system()` on POSIX — falls through to unsupported.  

### 2. lexer.cpp — Unterminated Strings/Includes
**Status: SAFE**  
- Unterminated `#include` stops at `\n` (line 92), path is discarded.  
- Unterminated string/raw string literals emit `TokenKind::Bad` instead of swallowing source.  
- Char literals with >1 codepoint are rejected as `TokenKind::Bad` (line 361).  

### 3. optimizer.cpp — Float Folding & Overflow
**Status: SAFE**  
- Float literals excluded from constant folding (`isFloatLiteral` check, line 39).  
- All arithmetic uses checked operations: `checked_mul`, overflow guards, `INT64_MIN` special-casing.  
- Negation of `INT64_MIN` is guarded (line 133).  
- Pow exponent capped at 62.  

### 4. parser.cpp — Syntax Safety
**Status: SAFE**  
- Dangling `@` at top-level is consumed to prevent infinite loops (line 1604-1609).  
- Colon-method hijack guarded by `inTernaryThen_` counter (line 419).  
- `>>`/`<<` tokens handled as double brackets in generic type annotations (line 2291-2301).  

### 5. eval.cpp — Division by Zero
**Status: SAFE**  
- `Div` and `Mod` both throw `std::runtime_error("Division by zero")` (lines 317-321).  

### 6. actions.cpp — Bounds Checking
**Status: SAFE**  
- All `forEach`, `get`, `set`, `push`, `pop`, etc. operations guard `mc.args.empty()` before access.  
- List operations use bounds-checked indices with safe fallbacks.  

---

## P1 — Incorrect Behavior (Resolved)

### 1. crypto.cpp — Thread Safety
**Status: SAFE**  
- `g_xor_state` is `std::atomic<uint32_t>` with explicit memory ordering (line 28).  

### 2. network.cpp — url_encode Zero-Padding
**Status: SAFE**  
- Already uses `std::setw(2) << std::setfill('0')` for proper hex encoding (line 461).  

### 3. regex.cpp — Match Mode
**Status: SAFE**  
- `re.match` uses `std::regex_match` (full match).  
- `re.find` uses `std::regex_search` (substring).  
- These are correctly distinct.  

### 4. typechecker.cpp — Module Builtins Leak
**Status: FIXED**  
- `builtins_` is cleared at start of each `check()` call (line ~973).  
- Performance and process modules now registered alongside existing modules.  

---

## P2 — Minor Issues

### 1. codegen_x64.cpp — Label Collision
**Status: MITIGATED**  
- Labels use per-function prefixes; `.LC` string constants use a global counter.  

### 2. ir.cpp — Break/Continue
**Status: MITIGATED**  
- Break/continue opcodes emitted; native codegen path rejects unsupported opcodes.  

### 3. core.cpp — State Reset
**Status: PARTIAL**  
- Global containers (`g_lists`, `g_dicts`, `g_ptrs`, `g_fileStreams`) are not explicitly reset between runs.  
- Workaround: each run uses incrementing IDs; old data is effectively orphaned.  

---

## Suggested Improvements

1. **Real Value Type**: Current `std::string`-backed values lose type information at runtime. A tagged union `Value` (int64, double, string, bool, null, handle) would simplify type checking and reduce conversion overhead.  

2. **Result Plumbing**: Replace `env.vars["_"]` magic variable with a proper return value channel (`ExecResult` struct containing value + error).  

3. **Per-Run State Reset**: Explicitly clear global container maps and file streams at the start of each `run()` invocation to prevent cross-run contamination.  

4. **CTest Harness**: Add a `.elan` golden-file test harness using CTest, comparing expected output files against actual runtime output.  

5. **Remove Dead Config**: The `SPLIT_RUNTIME_DLLS` CMake option and dead `--command` CLI arg in `main.cpp` should be removed to reduce build complexity.  

6. **Monitor Module**: The monitor builtins (experimental/) parse `"monitor:N"` handles with `atoi` — needs prefix stripping like threadmgr.  

7. **Thread Safety**: Worker threads in experimental `builtins/threads.cpp` capture raw `Runtime*`/`Program*` pointers — these should use `shared_ptr` or explicit lifetime guards.  

8. **File System Return Values**: Many FS operations (`fs.write`, `file_open`) return empty strings on failure rather than structured error information.  

---

## Files Verified

| File | Status |
|------|--------|
| `src/runtime/core.cpp` | OK |
| `src/runtime/eval.cpp` | OK (div-zero throws) |
| `src/runtime/actions.cpp` | OK (bounds-guarded) |
| `src/runtime/imports.cpp` | OK (modular aliases complete) |
| `src/runtime/builtins.cpp` | OK |
| `src/runtime/builtins/math.cpp` | OK |
| `src/runtime/builtins/system.cpp` | OK (no shell injection) |
| `src/runtime/builtins/network.cpp` | OK (url_encode padded, timeout) |
| `src/runtime/builtins/crypto.cpp` | OK (atomic state) |
| `src/runtime/builtins/regex.cpp` | OK (match vs search) |
| `src/runtime/builtins/performance.cpp` | OK |
| `src/runtime/builtins/binary.cpp` | OK |
| `src/runtime/builtins/data.cpp` | OK |
| `src/parser.cpp` | OK (ternary guard, >> handling, @ consumed) |
| `src/lexer.cpp` | OK (unterminated guards) |
| `src/optimizer.cpp` | OK (float excluded, checked math) |
| `src/typechecker.cpp` | OK (builtins cleared, all modules registered) |
| `src/codegen_x64.cpp` | OK (label prefixing) |
| `src/ir.cpp` | OK |
