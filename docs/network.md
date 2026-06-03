# Network

```elan
#include <builtin/network> as net
```

Alias: `builtin/net` works identically.

## HTTP

| Method | Args | Returns |
|--------|------|---------|
| `net.get(url)` | url | response body string, empty on failure |
| `net.post(url, body)` | url, body | response body string |
| `net.post(url, body, contentType)` | url, body, content-type header | response body string |
| `net.status(url)` | url | HTTP status code string (e.g. `"200"`) |
| `net.download(url, path)` | url, local path | `"true"` / `"false"` |
| `net.encode(s)` | string | URL-encoded string |

```elan
@erelang
#include <builtin/network> as net
#include <builtin/fs> as fs

public action main {
    let html = net.get("https://example.com");
    fs.write("page.html", html);
    print "Done";
}

run main;
```

### POST example

```elan
@erelang
#include <builtin/network> as net

public action main {
    let response = net.post("https://httpbin.org/post", "key=value");
    print response;
}

run main;
```

### HLS streaming

`net.hls_download_best(m3u8Url, outPath)` — downloads the highest-bandwidth HLS variant to a file. Returns `"true"` / `"false"`.

## Windows IP / DNS

These call `ipconfig` internally and return a result string.

| Builtin | Description |
|---------|-------------|
| `net.network.ip.flush()` | `ipconfig /flushdns` |
| `net.network.ip.release([adapter])` | release DHCP lease |
| `net.network.ip.renew([adapter])` | renew DHCP lease |
| `net.network.ip.registerdns()` | register DNS |

Result format: `success=true\nexit_code=0\noutput=...`

## Notes

- HTTP/HTTPS only (Windows WinHTTP). No proxy config needed for most cases.
- `net.get` returns empty string if the request fails.
- `net.status` sends a HEAD request — useful for checking reachability without downloading the body.

## Related

- [filesystem.md](filesystem.md)
- [automation.md](automation.md)
