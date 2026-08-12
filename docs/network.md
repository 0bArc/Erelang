# Network

```elan
#include <builtin/network> as net
```

Alias: `builtin/net` works identically.

## HTTP Client

| Method | Args | Returns |
|--------|------|---------|
| `net.get(url)` | url | response body string, empty on failure |
| `net.get_auth(url, header)` | url, auth header (e.g. `"Authorization: Bearer ..."`) | response body string |
| `net.get_resp(url)` | url | `resp:N` handle — status/body/headers via methods |
| `net.post(url, body)` | url, body | response body string |
| `net.post(url, body, contentType)` | url, body, content-type header | response body string |
| `net.post_auth(url, body, contentType, authHeader)` | url, body, content-type, auth | response body string |
| `net.put(url, body)` | url, body | response body string |
| `net.put(url, body, contentType)` | url, body, content-type | response body string |
| `net.put_auth(url, body, contentType, authHeader)` | url, body, content-type, auth | response body string |
| `net.patch(url, body, contentType)` | url, body, content-type | response body string |
| `net.patch_auth(url, body, contentType, authHeader)` | url, body, content-type, auth | response body string |
| `net.delete(url)` | url | response body string |
| `net.delete_auth(url, authHeader)` | url, auth header | response body string |
| `net.head(url)` | url | HTTP status code string |
| `net.status(url)` | url | HTTP status code string (HEAD request) |
| `net.download(url, path)` | url, local path | `"true"` / `"false"` |
| `net.encode(s)` | string | URL-encoded string |
| `net.json_encode(s)` | string | JSON-escaped string |
| `net.json_decode(s)` | string | raw string from JSON literal |

### resp: handle methods

When using `net.get_resp(url)`, you get a `resp:N` handle with these methods:

| Method | Returns |
|--------|---------|
| `handle.status()` | HTTP status code (int) |
| `handle.body()` | response body |
| `handle.header(name)` | value of header `name` |
| `handle.json()` | response body (same as `.body()`) |

```elan
net.get_resp("https://httpbin.org/get");
string resp = "" + _;
if (resp != "null") {
    resp.status();
    print "Status: " + _;
    resp.body();
    print "Body: " + _;
}
```

## HTTP Server

```elan
#include <builtin/network> as net
```

### Server creation

| Method | Args | Returns |
|--------|------|---------|
| `net.create_server(port)` | integer port | `http:N` handle |
| `net.create_server_tls(port, cert, key)` | port, cert path, key path | `http:N` handle |

### http: handle methods

| Method | Args | Description |
|--------|------|-------------|
| `server.get(path, actionName)` | route path, action name | register GET route |
| `server.post(path, actionName)` | route path, action name | register POST route |
| `server.put(path, actionName)` | route path, action name | register PUT route |
| `server.patch(path, actionName)` | route path, action name | register PATCH route |
| `server.del(path, actionName)` | route path, action name | register DELETE route |
| `server.ws(path, actionName)` | route path, action name | register WebSocket upgrade |
| `server.sse(path, actionName)` | route path, action name | register SSE route |
| `server.static(urlPrefix, dirPath)` | URL prefix, disk path | serve static files |
| `server.cors(origin)` | origin string (e.g. `"*"`) | set CORS |
| `server.rate_limit(prefix, max, windowSec)` | path prefix, max requests, window | rate limit |
| `server.use(actionName)` | middleware action name | add global middleware |
| `server.group()` | none | returns child `http:N` for group routing |
| `server.log_format(format)` | format string | set log format (`"combined"`) |
| `server.log_file(path)` | file path | enable file logging |
| `server.listen()` | none | start blocking accept loop (Ctrl+C to stop) |
| `server.shutdown_graceful(ms)` | timeout ms | graceful shutdown |

### Wildcard routing

Use `/*` suffix to match all paths under a prefix:

```elan
server.get("/api/*", "handle_api");
```

### Complete server example

```elan
@erelang
#include <builtin/network> as net

public action main {
    net.create_server("8080");
    string server = "" + _;

    server.cors("*");
    server.static("/static", "./public");
    server.rate_limit("/api", "30", "60");

    server.get("/", "home");
    server.get("/api/hello", "api_hello");
    server.post("/api/data", "api_data");
    server.ws("/chat", "ws_handler");
    server.sse("/events", "sse_handler");

    server.listen();
    print "Server stopped. Port 8080 released.";
}

public action home {
    res.html("<h1>Erelang Backend</h1>");
}

public action api_hello {
    res.json("{\"hello\":\"world\"}");
}

public action api_data {
    req.body();
    string data = "" + _;
    res.json(data);
}

run main;
```

## WebSocket

```elan
#include <builtin/websocket> as ws
```

Alias: `builtin/ws` works identically.

| Method | Args | Returns |
|--------|------|---------|
| `ws.connect(url)` | WebSocket URL | `ws:N` handle or `"null"` |
| `ws.send(id, data)` | raw handle id, data | `"true"` / `"false"` |
| `ws.recv(id)` | raw handle id | message string |
| `ws.recv_timeout(id, ms)` | raw handle id, timeout ms | message string |
| `ws.close(id)` | raw handle id | `"true"` |
| `ws.state(id)` | raw handle id | `"open"` / `"closed"` / `"connecting"` |
| `ws.broadcast(data)` | data to all connections | `"true"` |
| `ws.send_binary(id, data)` | raw handle id, binary data | `"true"` / `"false"` |

### ws: handle methods

After `ws.connect()`, use the handle directly:

| Method | Args | Returns |
|--------|------|---------|
| `sock.send(data)` | text data | `"true"` / `"false"` |
| `sock.send_binary(data)` | binary data | `"true"` / `"false"` |
| `sock.recv()` | none | message string |
| `sock.recv_timeout(ms)` | timeout ms | message string |
| `sock.broadcast(data)` | text data | `"true"` |
| `sock.close()` | none | `"true"` |
| `sock.close(code)` | close code (e.g. `1000`) | `"true"` |
| `sock.close(code, reason)` | close code, reason string | `"true"` |
| `sock.state()` | none | `"open"` / `"closed"` / `"connecting"` |

```elan
ws.connect("wss://echo.websocket.org");
string sock = "" + _;
if (sock != "null") {
    sock.send("hello");
    sock.recv();
    print "Echo: " + _;
    sock.close("1000", "done");
}
```

## Raw TCP

```elan
#include <builtin/tcp> as tcp
```

Alias: `builtin/rawtcp` works identically.

| Method | Args | Returns |
|--------|------|---------|
| `tcp.connect(host, port)` | hostname, port string | `tcp:N` handle or `"null"` |

### tcp: handle methods

| Method | Args | Returns |
|--------|------|---------|
| `sock.send(data)` | raw bytes | bytes sent (int) |
| `sock.recv()` | none | received data (string) |
| `sock.recv_timeout(ms)` | timeout ms | received data (string) |
| `sock.close()` | none | `"true"` |
| `sock.state()` | none | `"open"` / `"closed"` |

```elan
tcp.connect("example.com", "80");
string conn = "" + _;
if (conn != "null") {
    conn.send("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
    conn.recv_timeout("3000");
    print "Received " + tostr(string.len(_)) + " bytes";
    conn.close();
}
```

## HLS streaming

`net.hls_download_best(m3u8Url, outPath)`: downloads the highest-bandwidth HLS variant to a file.

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

- HTTP/HTTPS uses Windows WinHTTP.
- WebSocket uses WinHTTP WebSocket API.
- TCP uses raw Winsock2 sockets with DNS resolution.
- SSE routes keep connections open for server-sent events. The `sse:` handle supports `.emit(event, data)` and `.close()`.
- All handles return `"null"` on failure.
- HTTP server supports graceful Ctrl+C shutdown via `SetConsoleCtrlHandler`.
- Client sockets get a 5-second receive timeout to prevent slowloris attacks.

## Related

- [syntax.md](syntax.md)
- [filesystem.md](filesystem.md)
- [threads.md](threads.md)
