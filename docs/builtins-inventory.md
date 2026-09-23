# Builtins inventory

Classification of former/current callable names after the public-API redesign.

Legend:
- LANGUAGE PRIMITIVE — always available language surface
- PUBLIC STD API — import-gated module methods (`#include <std/…>` / `<builtin/…>`)
- INTERNAL RUNTIME PRIMITIVE — C++ dispatch only; TC rejects bare name
- DEBUG/TEST ONLY — diagnostics / deterministic harness
- LEGACY/REMOVE — rejected by typechecker; no public registration

| Name | Class |
|------|-------|
| `print` / `sleep` / `input` / `fail` | LANGUAGE PRIMITIVE |
| `now_ms` / `now_iso` / `env` / `uuid` / `rand_int` / `args_*` / `exit` / `exec` / `spawn` | LANGUAGE PRIMITIVE |
| `int` / `float` / `string` / `bool` | LANGUAGE PRIMITIVE (type constructors) |
| `alloc` / `free` / `realloc` / `copy` / `move` / `fill` / `zero` | LANGUAGE PRIMITIVE (`*T`) |
| `sizeof` / `alignof` / `__builtin_*` | LANGUAGE PRIMITIVE |
| `heap` / `shared` / `weak` / `buffer` | LANGUAGE PRIMITIVE (typed constructors) |
| `mutex_*` | LANGUAGE PRIMITIVE |
| `map` / `filter` / `reduce` / `set_*` | LANGUAGE PRIMITIVE (collections) |
| `string.*` methods + `text.lower()` style | LANGUAGE PRIMITIVE |
| `json.*` / `to_json` / `from_json` | LANGUAGE PRIMITIVE |
| `pipe.new` / `p.send` / `p.receive` / `p.close` | PUBLIC STD API (`std/pipe`) |
| `chan_*` | INTERNAL RUNTIME PRIMITIVE |
| `fut.cancel` / `fut.done` / `fut.cancelled` / `fut.result` / `await` | LANGUAGE PRIMITIVE (methods + syntax) |
| `future_cancel` / `future_done` | LEGACY/REMOVE |
| `fs.open` / `fs.read` / `fs.write` / handle `.read/.write/.flush/.close` | PUBLIC STD API (`std/fs`) |
| `file_open` / `file_read` / … / `fopen` / … | INTERNAL RUNTIME PRIMITIVE / LEGACY/REMOVE (bare) |
| `read_text` / `write_text` / path helpers | INTERNAL (bound as `fs.*` / `path.*`) |
| `crypto.sha256` / `crypto.aes_encrypt` / `crypto.aes_decrypt` | PUBLIC STD API (`std/crypto` / `builtin/crypto`) |
| `hash_sha256` / `aes_encrypt` / … | INTERNAL RUNTIME PRIMITIVE |
| `net.*` / `ws.*` / `tcp.*` | PUBLIC STD API (network modules) |
| `http_*` bare | INTERNAL / import-gated legacy forms |
| `process.*` / `system.*` | PUBLIC STD API |
| `toint` / `tostr` / `tofloat` / `tobool` | LEGACY/REMOVE |
| `strbuf_*` | LEGACY/REMOVE (use string methods / `buffer`) |
| `ptr_new` / `ptr_get` / `ptr_set` / `make_unique` / `make_shared` / `malloc` / `memcpy` | LEGACY/REMOVE |
| `own` / `own<T>` | LEGACY/REMOVE (renamed to `heap`) |
| `debug.*` | DEBUG/TEST ONLY |
| `advance_time` / deterministic seed | DEBUG/TEST ONLY |
| `plugin_core*` | LANGUAGE PRIMITIVE (plugin host) |
| `req.*` / `res.*` / `sse.*` / `resp.*` | PUBLIC STD API (handle methods) |

Internal C++ tables may still implement `chan_*` / `file_*`; user code must not call those names.
