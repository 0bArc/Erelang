# Docker plugin

Drive the docker CLI from Erelang: list containers and images, run, stop and remove containers,
read logs, exec into containers, inspect them as maps and run compose projects.

Install it by putting this folder in `plugins/` next to the Erelang executable. Its actions are then
available in every script. They all start with `docker_`.

```elan
@erelang

public action main() {
    if (!docker_running()) {
        print "start Docker Desktop first";
        return;
    }
    string id = docker_run("nginx", "-p 8080:80 --name web");
    if (!docker_wait_healthy(id, 30000)) {
        print docker_logs(id, 50);
        return;
    }
    for (string row : docker_ps(false)) {
        Map<string, string> c = from_json(row);
        print c["Names"] + "  " + c["Status"];
    }
    docker_rm(id, true);
}
run main;
```

## Actions

| Action | Returns |
|---|---|
| `docker_available()` | the CLI is installed |
| `docker_running()` | the daemon answers |
| `docker_version()` | server version, or `""` |
| `docker_ps(all)` | containers as JSON objects, one string each |
| `docker_names()` | names of running containers |
| `docker_images()` | images as JSON objects |
| `docker_run(image, args)` | container id, or `""` |
| `docker_start(id)` / `docker_stop(id)` / `docker_restart(id)` | success |
| `docker_rm(id, force)` | success |
| `docker_pull(image)` | success |
| `docker_logs(id, tail)` | last `tail` lines (0 means all) |
| `docker_exec(id, command)` | output of the command |
| `docker_inspect(id)` | the inspect object as a map (use `json.stringify` to print it) |
| `docker_field(id, template)` | one value through a Go template |
| `docker_status(id)` | `running`, `exited`, ... |
| `docker_wait_healthy(id, timeoutMs)` | became healthy (or running, without a health check) in time |
| `docker_compose_up(dir)` / `docker_compose_down(dir)` | success |
| `docker_cli(args)` | raw `docker <args>` output |
| `docker_ok()`, `docker_last_exit()`, `docker_last_output()` | result of the last call |

Arguments are passed to the shell as written, so quote values with spaces yourself.

Set `DOCKER_BIN` to use another CLI (for example `podman`), or assign `docker_bin` in a script.

## Testing

```
powershell -File plugins\docker\tests\run.ps1 -Exe build\release\bin\Release\erelang.exe
```

The test puts `tests/fake/docker.cmd` first on `PATH`, so it needs no Docker installation. The fake
logs every call, and the runner checks the exact command lines the plugin produced.
