# AI

`builtin/ai` writes and reads patterns for you, so you rarely type a regex by hand. Give it
examples and it infers the pattern, describe what you want in English and it builds the regex,
or hand it an existing regex and it explains or simplifies it.

It runs fully offline: a deterministic rules engine, no network, no key, the same answer every time.

```elan
#include <builtin/ai> as ai
```

```elan
@erelang
#include <builtin/ai> as ai

public action main() {
    // Infer a regex from samples.
    string rx = ai.auto(["ORD-0042-QA", "ORD-1937-QA", "ORD-8521-QA"]);
    print rx;                                   // ORD-[0-9]{4}-QA

    // Describe it in English.
    string email = ai.regex.generate("Match a corporate email address ending in .com");
    print ai.regex.test(email, "me@corp.com");  // true

    // Understand and simplify an existing pattern.
    print ai.regex.explain(rx);
    print ai.regex.optimize("[0-9][0-9][0-9]"); // \d{3}
}
run main;
```

Most calls come in two spellings of the same idea: a flat name (`ai.kind`) and a grouped name
(`ai.regex.generate`, `ai.infer.regex`). Both are plain function calls.

## Infer from samples

| Call | Returns |
|------|---------|
| `ai.auto(samples)` | regex for the samples. A known format (email, date, uuid...) gives its canonical regex, anything else is inferred |
| `ai.infer.regex(samples)` | regex inferred from a list of samples |
| `ai.infer.pattern(samples)` | [expect pattern](regex.md#expect-patterns) that covers the samples |
| `ai.pattern(sample)` | expect pattern for one sample |

`samples` is an `Array<string>`. A single string counts as one sample.

How inference works:

- Digit runs become `[0-9]{n}`, or `[0-9]{2,5}` when the samples differ in length.
- A letter run that never changes (`ORD`, `QA`) stays literal; one that varies becomes `[A-Za-z]{n}`.
- Separators such as `-`, `_`, `@`, `.` stay literal and are escaped.
- Samples with different shapes keep the head and tail they share and put `.*` between.

```elan
print ai.infer.regex(["user_001@example.com", "user_002@example.com"]);
// user_[0-9]{3}@example\.com
print ai.infer.pattern(["AB-1234", "CD-5678"]);       // XX-xxxx
```

## Generate from English

`ai.regex.generate(description)` returns a regex that matches the **whole** text, or an empty
string when it does not understand the description. `ai.regex.can_generate(description)` tells
you which in advance.

```elan
print ai.regex.generate("3 digits then a dash then 4 letters");          // \d{3}-[A-Za-z]{4}
print ai.regex.generate("starts with ORD- followed by 4 digits");        // ORD-\d{4}.*
print ai.regex.generate("an optional minus sign then one or more digits");  // -?\d+
print ai.regex.generate("contains the word \"error\"");                  // .*error.*
print ai.regex.generate("case insensitive \"hello\"");                   // [hH][eE][lL][lL][oO]
```

What the phrase grammar understands:

| Kind | Words |
|------|-------|
| Formats | email, url / link, ip address, ipv6, mac address, uuid / guid, date, time, date and time, hex color, hex number, semantic version, phone number, zip code, card number, slug, integer, decimal, boolean, word |
| Characters | digit, letter, lowercase letter, uppercase letter, hex digit, alphanumeric, word character, whitespace, space, tab, character, symbol, dash / hyphen, underscore, dot / period, comma, colon, slash, at sign, plus sign, minus sign |
| Counts | `3`, `three`, `exactly 3`, `at least 2`, `at most 5`, `up to 5`, `2 to 4`, `between 2 and 4`, `one or more`, `zero or more`, `any number of`, `or more` |
| Order | `then`, `followed by`, a comma |
| Choice | `or` (`a digit or a letter`) |
| Optional | `optional`, `optionally`, `maybe` before a piece |
| Anchors | `starts with`, `begins with`, `ends with`, `ending in`, `contains`, `containing` |
| Flags | `case insensitive`, `ignoring case` |
| Literals | text in quotes (`"ORD-"`), or a token with punctuation such as `.com` or `@` |

Put literal words in quotes. A bare word that the grammar does not know (`banana`) makes the
whole description fail rather than guess.

An `ending in` clause after a format is smart: `email address ending in .com` replaces the end of
the email pattern instead of tacking `.*.com` on.

## Understand a regex

| Call | Returns |
|------|---------|
| `ai.regex.explain(rx)` | plain English description |
| `ai.regex.optimize(rx)` | shorter equivalent regex |
| `ai.regex.test(rx, text)` | `true` if the whole text matches |
| `ai.regex.valid(rx)` | `true` if the regex compiles |
| `ai.regex.examples(rx, n)` | list of `n` strings the regex accepts (same list every run) |
| `ai.regex.of(kind)` | canonical regex of a known format |

```elan
print ai.regex.explain("user_[0-9]{3}@example\\.com");
// Matches the text "user_", then exactly 3 digits, then the text "@example.com".

print ai.regex.optimize("(?:a|b|c)-[0-9]{1}[0-9]{2}");   // [a-c]-\d{3}
```

`optimize` merges repeated classes into counts (`[0-9][0-9]` into `\d{2}`), turns single character
alternations into classes, removes useless groups and `{1}`, and shortens classes. Before it
returns a result it generates sample strings from both patterns and checks they accept each other.
If anything disagrees, or the regex uses syntax the engine does not model, you get the input back
unchanged.

The parser covers literals, escapes, `.`, `[classes]`, groups, `(?:...)`, `|`, `* + ? {n,m}` and
`^ $`. Lookaround, backreferences and `\b` are reported as not covered.

## Text helpers

| Call | Returns |
|------|---------|
| `ai.kind(text)` | what the text looks like |
| `ai.kind_all(samples)` | the most common kind among the samples |
| `ai.validate(kind, text)` | `true` if the whole text is of that kind |
| `ai.extract(text, kind)` | list of every match found inside longer text |
| `ai.count(text, kind)` | how many matches |
| `ai.redact(text, kind)` | text with matches replaced by `[KIND]` |
| `ai.redact_all(text)` | redacts emails, urls, uuids, IPs, MACs, card numbers and phone numbers |
| `ai.names()` | list of known kinds |
| `ai.describe(kind)` / `ai.example(kind)` | description and a sample of a kind |

```elan
string note = "Mail me@site.io, server 10.0.0.7";
Array<string> mails = ai.extract(note, "email");     // ["me@site.io"]
print ai.redact_all(note);                           // Mail [EMAIL], server [IPV4]
```

Known kinds: `bool`, `int`, `float`, `hex`, `hexcolor`, `ipv4`, `ipv6`, `mac`, `uuid`, `date`,
`datetime`, `time`, `email`, `url`, `semver`, `phone`, `card`, `zip`, `slug`, `word`. `ai.kind`
also answers `empty` and `text`.

!!! note "Quantifier braces in strings"
    In a string literal, `{4}`, `{2,5}` and `{2,}` are kept as written, so regex quantifiers do
    not turn into interpolation. A backslash must be doubled in a literal: `"\\d+"`.

A runnable version is in `examples/std/ai.elan`, and `tests/std/ai.elan` checks every function.
