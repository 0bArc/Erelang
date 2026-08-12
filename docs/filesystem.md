# Filesystem

```elan
#include <builtin/fs> as fs
#include <builtin/path> as path
```

`builtin/path` alone gives path helpers + `file_exists`. For read/write/copy/list, import `builtin/fs`.

## High-level API

Whole-file helpers: best for scripts.

| Alias | Builtin | Args | Returns |
|-------|---------|------|---------|
| `fs.read(path)` | `read_text` | path | file contents string |
| `fs.write(path, text)` | `write_text` | path, body | void |
| `fs.append(path, text)` | `append_text` | path, body | void |
| `fs.exists(path)` | `file_exists` | path | `bool` (file **or** directory) |
| `fs.is_dir(path)` | `is_dir` | path | `bool` |
| `fs.is_file(path)` | `is_file` | path | `bool` (regular file) |
| `fs.mkdir(path)` | `mkdirs` | path | void (creates parents) |
| `fs.copy(src, dst)` | `copy_file` | src, dst | `bool` |
| `fs.move(src, dst)` | `move_file` | src, dst | `bool` |
| `fs.remove(path)` | `delete_file` | path | `bool` |
| `fs.list(dir)` | `list_files` | directory | `Array<string>` of child paths |
| `fs.dirs(dir)` | `list_dirs` | directory | `Array<string>` of subdirectory paths |
| `fs.files(dir)` | `list_regular_files` | directory | `Array<string>` of regular file paths |
| `fs.size(path)` | `file_size` | path | bytes as int (`-1` on error) |
| `fs.mtime(path)` | `file_mtime` | path | Unix seconds as int (`0` on error) |
| `fs.cwd()` | `cwd` | | current directory |
| `fs.chdir(path)` | `chdir` | path | `bool` |

`fs.list` / `fs.dirs` / `fs.files` throw if the path is missing or not a directory (they do not return an empty list for typos).

`fs.read` / `fs.write` are for **file content only**. To list a directory, use `fs.list`, `fs.dirs`, or `fs.files`. Prefer `fs.exists(p) && fs.is_dir(p)` when you mean a directory.

## Examples

### Read a file

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    body = fs.read("config.txt");
    print body;
}

run main;
```

### Write a file

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    fs.write("output.txt", "hello\n");
    fs.append("output.txt", "world\n");
    print "written";
}

run main;
```

### Check existence and backup

```elan
@erelang
#include <builtin/fs> as fs
#include <builtin/path> as path

public action main {
    cfg = path.join("config", "app.ini");
    if (!fs.exists(cfg)) {
        stderr_print "missing config";
        exit(1);
    }
    body = fs.read(cfg);
    fs.write(path.join("config", "app.ini.bak"), body);
    print "backup created";
}

run main;
```

### List a directory

`fs.list` returns path strings. Bind the loop variable as `string`:

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    Array<string> entries = fs.list("examples");
    for (string f in entries) {
        print f;
    }
}

run main;
```

### Subcommand folders (dirs only)

```elan
@erelang
#include <builtin/fs> as fs
#include <builtin/path> as path

public action main {
    if (fs.exists("commands") && fs.is_dir("commands")) {
        for (string subFolder in fs.dirs("commands")) {
            string name = path.name(subFolder);  // mod, dev, ...
            for (string file in fs.files(subFolder)) {
                if (path.ext(file) == ".elan") {
                    print(file);
                }
            }
        }
    }
}

run main;
```

Equivalent without filters:

```elan
for (string entry in fs.list("commands")) {
    if (!fs.is_dir(entry)) continue;
    string name = path.name(entry);
}
```

## Path helpers

Available via `fs` or `path` import:

| Alias | Builtin | Notes |
|-------|---------|-------|
| `path.join(a, b, ...)` | `path_join` | variadic |
| `path.parent(p)` / `path.dirname(p)` | `path_dirname` | |
| `path.name(p)` / `path.basename(p)` | `path_basename` | |
| `path.ext(p)` | `path_ext` | includes dot, e.g. `.elan` |
| `path.exists(p)` | `file_exists` | |

```elan
@erelang
#include <builtin/path> as path

public action main {
    p = path.join("examples", "program.elan");
    print "dir={path.dirname(p)}";
    print "name={path.name(p)}";
    print "ext={path.ext(p)}";
}

run main;
```

## Metadata

| Alias / Builtin | Returns |
|-----------------|---------|
| `fs.size(path)` / `file_size(path)` | bytes as int, `-1` on error |
| `fs.mtime(path)` / `file_mtime(path)` | Unix seconds as int; `0` on error (ambiguous with epoch — check `fs.exists` first) |

`fs.list` / `fs.dirs` / `fs.files` throw if the path is missing or not a directory (they do not return an empty list for typos).

## Streaming handles

For reading/writing in chunks without loading the whole file:

```elan
@erelang
#include <builtin/fs> as fs

public action main {
    h = file_open("out.bin", "wb");
    file_write(h, "chunk1");
    file_write(h, "chunk2");
    file_flush(h);
    file_close(h);
}

run main;
```

See [low-level.md](low-level.md) for the full handle API.

## Notes

- Relative paths resolve against the directory of the running script.
- On Windows use forward slashes `"examples/out.txt"` or escaped backslashes `"examples\\out.txt"` in string literals.

## Related

- [low-level.md](low-level.md)
- [automation.md](automation.md)
- [monitor.md](monitor.md)
