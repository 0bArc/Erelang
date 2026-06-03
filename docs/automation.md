# Automation

Erelang is good for glue work: transform files, run shell commands, download via HTTP, watch folders, persist small state.

## Skeleton

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    let root = fs.cwd();
    print "cwd={root}";
}

run main;
```

## Common patterns

### Read → transform → write

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    let text = fs.read("input.log");
    fs.write("output.log", text);
    print "done";
}

run main;
```

### Walk a directory

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    let files = fs.list("inbox");
    let n = list_len(files);
    let i = 0;
    while (i < n) {
        let f = list_get(files, i);
        print f;
        i = i + 1;
    }
}

run main;
```

### Run an external command

```elan
@erelang

public action main {
    let code = exec("cmd /c dir /b examples");
    print "exit={code}";
}

run main;
```

### Capture command output

```elan
@erelang
#include <builtin/system> as sys

public action main {
    sys.execute("whoami");
    print sys.output();
}

run main;
```

### Read an environment variable / CLI arg

```elan
@erelang

public action main {
    let profile = env("USERPROFILE");
    print "home={profile}";
    let first_arg = args_get(0);
    print "arg0={first_arg}";
}

run main;
```

### HTTP download

```elan
@erelang
#include <builtin/network> as net
#include <builtin/fs> as fs

public action main {
    let body = net.get("https://example.com");
    fs.write("page.html", body);
    print "saved";
}

run main;
```

### Loop until a stop file appears

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    while (true) {
        if (fs.exists("stop.flag") == "true") {
            break;
        }
        sleep 5000ms;
    }
    print "stopped";
}

run main;
```

### Your own `.elan` library

Put shared actions in a file and include it:

```elan
@erelang
#include <modules/utils.elan>

public action main {
    my_util_action("hello");
}

run main;
```

## Error handling

Typechecker **errors** (`TC001`, etc.) stop execution before the script runs. **Warnings** (`TC130` unused action, `TC120` unused var) still run.

See [diagnostics.md](diagnostics.md).

## Related docs

| Task | Doc |
|------|-----|
| Files | [filesystem.md](filesystem.md) |
| Shell | [process.md](process.md) |
| HTTP | [network.md](network.md) |
| Collections | [collections.md](collections.md) |
| Regex | [regex.md](regex.md) |
