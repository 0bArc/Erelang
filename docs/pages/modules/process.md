# Process and shell

## Core: no import needed

| Builtin | Args | Returns |
|---------|------|---------|
| `exec(cmd)` | full command line | exit code as string |
| `spawn(cmd)` | command line | process id (detached, fire-and-forget) |
| `exit(code)` | int | terminates script immediately |

```elan
@erelang

public action main {
    code = exec("cmd /c echo hello");
    print "exit={code}";
}

run main;
```

Open a URL or file with its registered app:

```elan
@erelang

public action main {
    exec("cmd /c start https://example.com");
    exec("cmd /c start notepad.exe report.txt");
}

run main;
```

## `builtin/system`: capture output

```elan
#include <builtin/system> as sys
```

| Call | Notes |
|------|-------|
| `sys.execute(program, arg?, dir?)` | run one program without a shell (no quoting surprises), capture stdout+stderr; returns exit code. `arg` is passed as a single argument |
| `sys.output()` | text from last `execute` call |
| `sys.last_exit()` | exit code from last `execute` call |
| `sys.cmd(line, dir?)` | run a full command line through the shell (`cmd.exe` on Windows, `sh` elsewhere), returns the output. Use this for commands with several arguments such as `docker run -d -p 80:80 nginx` |
| `sys.os()` | `"windows"`, `"linux"` or `"macos"`. Use it to pick shell commands that differ per platform |
| `sys.ip.flush()` | flush the DNS cache (`ipconfig /flushdns`, `resolvectl flush-caches` or `dscacheutil -flushcache`); `true` on success |

```elan
@erelang
#include <builtin/system> as sys

public action main {
    sys.execute("whoami");
    out = sys.output();
    code = sys.last_exit();
    print "user={out}";
    print "exit={code}";
}

run main;
```

## When to use which

| Need | Use |
|------|-----|
| Fire-and-forget, only exit code matters | `exec` |
| Need stdout text in the script | `builtin/system` |
| Background process, no wait | `spawn` |

## Related

- [automation.md](../advanced/automation.md)
- [network.md](network.md)
- [core-builtins.md](core-builtins.md)
