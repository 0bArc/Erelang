# Discord plugin rewrite (2026-10-21)

## What was wrong

The old `plugins/discord` was written for a language dialect that no longer exists, and it did
not run:

- It did not type-check: untyped parameters, `ws_connect`/`ws_recv` as free functions, string
  concatenation with ints, and `on_discord_interaction` called but never defined.
- The manifest named `discord_init` and `discord_shutdown` hooks that did not exist.
- Plugin scripts ending in `.elan` were never loaded at all. `src/plugins.cpp`
  (`builtin_script_extensions`) only accepted `.0bs`, `.erelang` and `.obsecret`, so every
  plugin action was "unknown" in user scripts.
- JSON was built and read with naive string slicing, so quotes and newlines in message text broke
  the payload, and the login code guessed whether to prefix `Bot ` from the token's first letters.
- The gateway ignored RESUME, RECONNECT, INVALID_SESSION and close codes.

## What changed

Plugin (`plugins/discord/`):

- `api.elan`: REST client (`discord_request` plus get/post/patch/put/delete), retry on HTTP 429
  using `retry_after`, JSON escaping through `net.json_encode`, messages, embeds, slash command
  registration (guild or global), interaction replies (normal and ephemeral), `discord_init` and
  `discord_shutdown` hooks. `discord_init` reads `DISCORD_TOKEN`, `DISCORD_CLIENT_ID`,
  `DISCORD_GUILD_ID` from the environment.
- `gateway.elan`: session loop with HELLO, IDENTIFY, heartbeat with ACK tracking, RESUME,
  RECONNECT, INVALID_SESSION, fatal close codes (4004, 4010 to 4014), event dispatch to handlers
  registered by name with `discord_on_ready/message/command/event`. `discord_run` and
  `discord_stop`.
- `slash.elan`: typed option builders and `discord_options1..4`.
- `init.core`: non-secret defaults only. `project.elp`: version 2.0.0.
- `README.md`, and `tests/` with `mock_discord.py` (REST and gateway mock), `bot_smoke.elan`
  and `run.ps1` (9 checks on recorded traffic, all pass).

Language and runtime gaps found while doing it:

- `src/plugins.cpp`: `.elan` is now a plugin script extension.
- `from_json` keeps nested objects and arrays as raw JSON text (`serialization.cpp`) and the
  typechecker types it as `map<string,string>`.
- Expression form method calls on network handles (`string m = sock.recv_timeout(100);`) now work
  (`dispatch_value_method` in `eval.cpp`). The old tests passed only because they used statement
  form, which discards the result. This also fixed `tests/std/udp_loopback.elan` printing nothing.
- WebSocket client (`builtins/websocket.cpp`): reader thread with an inbox, so `recv_timeout`
  really times out; fragmented message reassembly; `close_code()` and `close_reason()`.
- Typechecker: `Array<string> parts;` declarations (TC055 false positive), and
  `for (string k, string v : map)` now types `v`.
- `string.replace(s, from, to)` and `string.split(s, sep)` were declared but not implemented.
  Both are now implemented in `builtins.cpp`.
- Globals written by a callee were invisible to the caller, and a callee's stale copy could
  overwrite a nested call's update. `Runtime::run`, `run_single_action`, `call_action_by_name`
  and the scripted action path in `eval_builtin_call` no longer copy globals in or out; reads fall
  back to `globalVars_` and writes go straight to it. The plugin relies on this
  (`discord_stop()` called from inside a handler).

## Behavior changes

- Callee writes to a global are visible to the caller immediately.
- `.elan` files listed in a plugin manifest are loaded and merged into every script run.
- `from_json` on nested JSON returns the nested part as text instead of failing.
- Docs: `strings.md`, `core-builtins.md`, `network.md`, `collections.md` updated.
  `syntax.md` examples used a top level `const`, which is a syntax error; they now use `global`.

## Verification

- `tests\run.ps1` on the Release build: 74 pass, 0 fail.
- `plugins\discord\tests\run.ps1`: 9 of 9 checks pass (bot auth header, IDENTIFY with intents 513,
  heartbeat, RESUME after RECONNECT, 429 retry, interaction callback, bot message ignored, option
  value with quotes decoded, clean stop).
- Golden stdout diff against the pristine baseline: only the known noisy tests differ
  (`random`, `stdio` ordering, `heap_move` intentional fix) plus `udp_loopback`, where the
  baseline lost the received text and the new build prints it.
- Benchmarks unchanged: loop 85 ms, fib 32 ms, list 82 ms, dict 79 ms.

## Not done

- Not tested against the real Discord service (no token used). Behavior follows the documented
  gateway v10 protocol and the mock.
- No sharding, voice, or message component interactions. Only chat input commands (type 2).
- Per call `sleep` still takes literals only, so waits poll in 20 ms steps.
- `.core` values are not readable from scripts without the `import "/plugins/*/project.elp"`
  alias; the plugin therefore uses environment variables for settings.
