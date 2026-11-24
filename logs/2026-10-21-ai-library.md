# builtin/ai and re.expect (2026-10-21)

## What was wrong or missing

- Matching text meant hand written regexes. Large patterns were hard to read, and common checks
  (digits, dates, emails) were rewritten in every script.
- A string literal such as `"[0-9]{4}"` lost its braces: `{4}` was evaluated as interpolation and
  came out as `4`, so quantifiers could not be written in a literal.
- `Array<string> a = ["007", "3.14"]` stored numbers: list literals turned numeric looking string
  literals into ints and floats ("007" became 7). Pre-existing, found while testing.
- The plugin loader and parser had no notion of a module with nested names, so `ai.regex.generate(...)`
  could not be a call.

## What changed

- `builtin/regex` gained `re.expect(pattern, text)` (readable patterns: `x` digit, `X` letter, `?` any
  char, `*` any run, backslash escapes), `re.explain(pattern)` and `re.infer(sample)`.
- New module `builtin/ai` (`src/runtime/builtins/ai*.cpp`), offline and deterministic:
  - `ai_infer.cpp`: `ai.auto`, `ai.infer.regex`, `ai.infer.pattern`, `ai.pattern`.
  - `ai_nl.cpp`: `ai.regex.generate` / `can_generate`, a phrase grammar for English descriptions.
  - `ai_regex_tools.cpp`: a regex parser and tree, `ai.regex.explain`, `optimize` (with an
    equivalence check on generated samples), `examples`, `test`, `valid`.
  - `ai_text.cpp`: the table of known formats (20 kinds) and `kind`, `extract`, `count`, `validate`,
    `redact`, `redact_all`, list helpers.
  - `ai.cpp`: dispatch. An internal header `ai_internal.hpp` shares the interface.
- Nested names: the parser folds `alias.group.method(...)` into one dotted call name when the alias
  is a `builtin/ai` import (`src/parser.cpp`), and `imports.cpp` resolves names such as
  `regex.generate` to flat builtins `ai_regex_generate`. Typechecker signatures were added.
- `eval_string`: `{4}`, `{2,5}` and `{2,}` stay literal.
- List literals keep string literals as strings.
- Removed the short lived `builtin/intelligence` module; `builtin/ai` replaces it.
- Docs: `docs/pages/modules/ai.md`, `regex.md` (expect patterns, `re.infer`), nav and home card.
- Tests: `tests/std/ai.elan` (about 90 checks, including a loop that validates examples and the
  optimized form of every known kind), `tests/std/regex_expect.elan`. Examples: `examples/std/ai.elan`,
  `examples/std/regex_expect.elan`.

## Behavior changes

- `{digits}` and `{digits,digits}` in string literals are no longer interpolation.
- String literals in list literals are strings ("007" stays "007").
- A lowercase `x` in a `re.expect` pattern means a digit; write `\x` for a literal x.

## Verification

- `tests\run.ps1`: 76 pass, 0 fail (Release). Discord plugin test still 9 of 9.
- `docs`: `python -m mkdocs build` with no warnings.

## Not done

- The generator is a rules engine, not a language model. Words outside its grammar (for example a
  bare unknown word) make `ai.regex.generate` return an empty string instead of guessing.
- `ai.regex.explain` and `optimize` do not model lookaround, backreferences or `\b`.
- `phone`, `card` and `ipv6` are shape checks, not validators (no Luhn, IPv6 full form only).

## Follow up: console garbage and multi-line calls

- Diagnostics printed `ΓÇö` because compiler messages contained an em dash (UTF-8 bytes E2 80 94),
  which a Windows console in an OEM code page shows as those three characters. All em and en dashes
  in `src` and `include` (messages and comments) were replaced with plain ASCII.
- A call whose arguments start on a new line (`string out = ai.regex.generate(` then the string on
  the next line) was a syntax error for every call, not just `ai`. Argument lists now skip newlines
  after `(`, around each argument and before `)` (`src/parser.cpp`, six sites). Test:
  `tests/std/multiline_call.elan`.
