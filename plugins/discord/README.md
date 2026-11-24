# Discord plugin

A small Discord bot toolkit written in Erelang. It talks to the REST API, keeps a gateway
(WebSocket) connection alive, and hands events to handlers that your script registers.

The plugin is loaded automatically when it sits in the `plugins/` folder next to the Erelang
executable. Its actions are then available in every script. All of them start with `discord_`
or `slash_`, so they do not clash with your own names.

## Quick start

```
@erelang

public action on_message(channel_id: string, author_id: string, author_name: string, content: string, message_id: string) {
    if (content == "!ping") {
        discord_send_message(channel_id, "pong");
    }
}

public action main() {
    discord_login(env("DISCORD_TOKEN"));
    discord_on_message("on_message");
    discord_run();
}
run main;
```

Never put a real token in a file that is committed. The `discord_init` load hook reads
`DISCORD_TOKEN`, `DISCORD_CLIENT_ID` and `DISCORD_GUILD_ID` from the environment, so a script
that only calls `discord_run()` already works when those are set.

## Files

| File | Purpose |
|---|---|
| `api.elan` | REST client, JSON helpers, messages, slash command registration, interaction replies, load and unload hooks |
| `gateway.elan` | Gateway session: hello, identify, heartbeat, resume, reconnect, event dispatch |
| `slash.elan` | Option builders and `discord_options1..4` |
| `init.core` | Informational defaults (API base, default intents) |
| `tests/` | Mock Discord server and an end to end test |

## Handlers

Register an action by name. Handlers are optional.

| Register with | Handler signature |
|---|---|
| `discord_on_ready("h")` | `h(user_id: string, user_name: string)` |
| `discord_on_message("h")` | `h(channel_id, author_id, author_name, content, message_id: string)` |
| `discord_on_command("h")` | `h(name, options_json, interaction_id, interaction_token, user_id: string)` |
| `discord_on_event("h")` | `h(event_type: string, data_json: string)`, called for every dispatch |

Bot authored messages are ignored by default (`discord_ignore_bots = 0` disables that).
Read a slash command option with `discord_option(options_json, "name")`.

## Actions

Setup: `discord_login`, `discord_set_client_id`, `discord_set_guild_id`, `discord_set_intents`.
Gateway: `discord_run` (blocks until `discord_stop()` or a fatal close code), `discord_stop`.
Messages: `discord_send_message`, `discord_send_embed`, `discord_edit_message`, `discord_delete_message`.
Commands: `discord_create_command`, `discord_respond`, `discord_respond_ephemeral`, `slash_register_ping`, `slash_register_echo`.
Low level: `discord_request(method, endpoint, body)`, `discord_get`, `discord_post`, `discord_patch`, `discord_put`, `discord_delete`, `discord_failed`.

Every REST helper returns the raw JSON body. A 429 response is retried after its `retry_after`
(up to `discord_max_retries`, default 3). Message content is JSON escaped for you.

Gateway behavior: default intents are 513 (guilds and guild messages; message content is a
privileged intent you must enable in the developer portal and add with
`discord_set_intents(513 + 32768)`). A dropped connection, `RECONNECT` or missed heartbeat ACK
resumes the session. An invalid session starts a fresh one. Close codes 4004, 4010 to 4014 are
fatal and make `discord_run` return.

## Testing

```
powershell -File plugins\discord\tests\run.ps1 -Exe build\release\bin\Release\erelang.exe
```

The test starts `tests/mock_discord.py` (Python 3 standard library only), runs
`tests/bot_smoke.elan` against it, and checks the recorded REST calls and gateway frames:
auth header, identify intents, heartbeat, resume, rate limit retry, slash command reply.
