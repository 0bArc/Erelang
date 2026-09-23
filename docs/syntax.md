# Erelang Syntax Reference

Complete language syntax reference covering all constructs, operators, types, and builtin modules.

## File Structure

```elan
@erelang                          // File directive (line 1)
@strict                           // Enable strict type checking
@entry("main")                    // Custom entry point

#include <builtin/network> as net // Import builtin with alias
#include <builtin/websocket> as ws
#include <builtin/fs> as fs

// --- Actions (functions) ---
public action home {
    res.html("<h1>Hello</h1>");
}

public action api_users {
    res.json(usersData);
}

public int add(a: int, b: int) {
    return a + b;
}

// --- Entities (objects/structs) ---
entity User {
    public string name;
    public int age;
    public action greet {
        print "Hi, " + self.name;
    }
}

// --- Globals ---
const VERSION = "1.0.0";
int counter = 0;

// --- Hooks ---
hook onStart { print "Starting..."; }
hook onEnd   { print "Done.";      }

// --- Run entry ---
run main;
```

## Actions (Functions)

Action declarations can use either style:

| Style | Example |
|---|---|
| Classic | `public action greet(name: string): string { ... }` |
| Type-first | `public string greet(string name) { ... }` |
| `->` syntax | `public action greet(name: string) -> string { ... }` |

```elan
public action no_return {       // void (no return type)
    print "side effect";
}

public action with_return: int { // returns int
    return 42;
}

public action params(a: int, b: string, c: bool) {
    print a + " " + b;
}

// Visibility
private action internal_helper { }  // Only callable within this file
public action external_api { }      // Callable from imports
```

- Parameters and return types are optional (defaults to `void`/no return).
- `public`/`private` enforced when `@strict` is enabled.
- Entry point: `run <actionName>` at file bottom.

## Variables

```elan
x = 2;              // Inferred type (int)
int y = 10;         // Typed declaration
string name = "Erelang";
bool flag = true;
double pi = 3.14159;
const PI = 3;       // Compile-time constant
```

Runtime values are string-backed; numeric operators parse operands as numbers.

## Types

| Type | Example | Notes |
|---|---|---|
| `int` | `42`, `-7` | 64-bit integer |
| `string` | `"hello"` | Double-quoted |
| `bool` | `true`, `false` | Booleans |
| `double` | `3.14`, `-2.5` | Floating point |
| `void` | (none) | No return value |
| `any` | any value | Explicit opt-in only |
| `struct:Name` | - | User-defined struct |
| `list:T` | `[1, 2, 3]` | Typed list |
| `map:K,V` | `{"a": 1}` | Typed dictionary |

Type constructors for conversion:
```elan
int x = int("42");          // "42" -> 42
string s = string(42);      // 42 -> "42"
bool b = bool("true");      // "true" -> true
double d = double("3.14");  // "3.14" -> 3.14
```

## Strings

```elan
print "Hello, World!";
print "Value: {x}";         // String interpolation
string s = "Hello " + "World";  // Concatenation with +
string len = "hello".len();     // Length via handle method: 5
string upper = "hello".upper(); // Uppercase via handle method: "HELLO"
```

## Control Flow

```elan
// if/else
if (x < 0) {
    print "negative";
} else if (x == 0) {
    print "zero";
} else {
    print "positive";
}

// while loop
while (i < 10) {
    i = i + 1;
}

// for loop (C-style)
for (i = 0; i < 10; i = i + 1) {
    print i;
}

// for-each (on lists)
for (item in myList) {
    print item;
}

// for-each (on dicts)
for (key, value in myDict) {
    print key + " = " + value;
}

// parallel block (concurrent execution)
parallel {
    background_task();
    compute_a();
    compute_b();
}
```

## Operators

### Arithmetic
```elan
a + b     a - b     a * b     a / b     a % b
```

### Comparison
```elan
a == b    a != b    a < b     a > b     a <= b    a >= b
```

### Logical
```elan
a && b    a || b    !a
```

### Ternary
```elan
string result = (x > 0) ? "positive" : "non-positive";
```

## Lists

```elan
list.push("item");
int count = list.len();
string item = list.get(0);
list.remove(0);
bool empty = list.is_empty();
list.clear();
list.pop();
```

Handles are strings with a `list:N` prefix.

## Dictionaries (Maps)

```elan
dict.set("key", "value");
string val = dict.get("key");
bool has = dict.has("key");
dict.remove("key");
int count = dict.len();
list keys = dict.keys();
list values = dict.values();
dict.clear();
```

Handles are strings with a `dict:N` prefix.

## Structs/Entities

```elan
struct Point {
    public int x;
    public int y;
    public action describe: string {
        return "Point(" + string(self.x) + ", " + string(self.y) + ")";
    }
}

// Usage
Point p;
p.x = 10;
p.y = 20;
string desc = p.describe();
```

Handles are strings with a `struct:Name` prefix.

## Null

`null`, `nil`, and `nullptr` are equivalent.

```elan
string data = null;
if (data == null) {
    print "no data";
}
```

## Attributes

```elan
@erelang              // Mark as Erelang source
@strict               // Enable strict mode (type checking, visibility)
@entry("main")        // Custom entry action name
@event("onClick")     // Event hook
@inline               // Inline hint for optimizer
@noinline             // No-inline hint
@hot                  // Hot path hint
@cold                 // Cold path hint
```

## Comments

```elan
// Single-line comment only
```

## Imports

```elan
#include <builtin/network> as net
#include <builtin/websocket> as ws
#include <builtin/fs> as fs
#include <builtin/math> as math
#include <builtin/crypto> as crypto
#include <builtin/threads> as thread
#include <builtin/system> as sys
#include <builtin/data> as data
#include <builtin/binary> as bin
#include <builtin/regex> as regex
#include <builtin/monitor> as monitor
#include <builtin/perm> as perm
#include "mylib.elan"   // Local import
```

Module methods are called with dot syntax:
```elan
string html = net.get("https://example.com");
int hash = crypto.sha256("hello");
thread.spawn("myTask");
```

## Handle System

Handles are string values with a prefix indicating their type:

| Prefix | Type | Created by |
|---|---|---|
| `list:N` | Collection list | `list.new()`, `[1, 2, 3]` |
| `dict:N` | Collection dictionary | `dict.new()`, `{"a": 1}` |
| `file:N` | File stream | `fs.open(path)` |
| `ws:N` | WebSocket connection | `ws.connect(url)` |
| `http:N` | HTTP server | `net.create_server(port)` |
| `req:N` | HTTP request | (injected in handler) |
| `res:N` | HTTP response | (injected in handler) |
| `sse:N` | SSE connection | (injected in handler) |
| `struct:Name` | Struct instance | `Name v;` |
| `ptr:N` | Raw pointer | `ptr.new()` |

Handle methods are called with dot syntax:
```elan
string file = fs.open("data.txt", "r");
string content = file.read();
file.close();

string ws = ws.connect("wss://echo.example.com");
ws.send("ping");
string msg = ws.recv_timeout("3000");
string st = ws.state();
ws.close();
```

## HTTP Client

```elan
#include <builtin/network> as net

// GET request
string html = net.get("https://api.example.com/data");

// GET with auth header
string data = net.get_auth("https://api.example.com/secure", "Bearer token123");

// POST request
string resp = net.post("https://api.example.com/submit", body, "application/json");

// POST with auth
string resp = net.post_auth("https://api.example.com/submit", body, "type/json", "Bearer token");

// PUT / PATCH with auth
string resp = net.put_auth(url, body, contentType, authHeader);
string resp = net.patch_auth(url, body, contentType, authHeader);

// DELETE with auth
string resp = net.delete_auth(url, authHeader);

// Check HTTP status code
string code = net.status("https://example.com");

// Download file
net.download("https://example.com/file.zip", "./downloads/file.zip");

// URL encode
string encoded = net.encode("hello world");  // "hello%20world"

// HLS stream download
net.hls_download_best("https://example.com/stream.m3u8", "./video.mp4");
```

## WebSocket Client

```elan
#include <builtin/websocket> as ws

public action main {
    string sock = ws.connect("wss://echo.example.com/chat");

    if (sock == null) {
        print "connection failed";
        return;
    }

    print "state: " + sock.state();     // "open"
    sock.send("Hello from Erelang!");

    string reply = sock.recv_timeout("3000");
    print "received: " + reply;

    sock.close();
}
```

WebSocket handle methods:
- `sock.send(data)` -- Send a text message
- `sock.recv()` -- Block until a message arrives, return it
- `sock.recv_timeout(ms)` -- Receive with timeout in milliseconds
- `sock.close()` -- Close the connection
- `sock.state()` -- Returns "connecting", "open", "closing", or "closed"

## HTTP Server

### Creating a server

```elan
#include <builtin/network> as net

public action main {
    // Basic server
    string server = net.create_server(8080);

    // TLS server
    string tlsServer = net.create_server_tls(443, "cert.pem", "key.pem");

    server.listen();
}
```

### Route handlers

Route handlers are named actions that receive `req` and `res` as injected handles:

```elan
public action home {
    res.html("<h1>Erelang Backend</h1>");
    res.end();
}

public action api_hello {
    string name = req.query("name");
    if (name == "") { name = "World"; }
    res.json("{\"hello\":\"" + name + "\"}");
    res.end();
}

public action api_create {
    string body = req.body();
    // process body...
    res.status("201");
    res.json("{\"created\":true}");
    res.end();
}

public action main {
    string server = net.create_server(8080);

    server.get("/", "home");
    server.get("/api/hello", "api_hello");
    server.post("/api/create", "api_create");
    server.put("/api/update", "api_update");
    server.del("/api/remove", "api_remove");

    server.listen();
}
```

### Request handle (req:N)

| Method | Returns | Description |
|---|---|---|
| `req.body()` | `string` | Full request body |
| `req.query("key")` | `string` | URL query parameter |
| `req.header("key")` | `string` | Request header value |
| `req.method()` | `string` | HTTP method (GET, POST, etc.) |
| `req.path()` | `string` | Request URI path |
| `req.cookie("name")` | `string` | Cookie value |
| `req.file("field")` | `string` | Uploaded file data |
| `req.save_upload("field", "./dir/")` | `string` | Save uploaded file to disk |

### Response handle (res:N)

| Method | Description |
|---|---|
| `res.html("<h1>Hi</h1>")` | Set HTML response |
| `res.json("{\"key\":\"val\"}")` | Set JSON response |
| `res.text("plain text")` | Set plain text response |
| `res.write("chunk")` | Append to response body (streaming) |
| `res.status("201")` | Set HTTP status code |
| `res.header("X-Custom", "val")` | Set response header |
| `res.cookie("name", "val", "3600", "/", "", "true")` | Set cookie (name, value, maxAge, path, domain, secure) |
| `res.end()` | Mark response as complete |

### Middleware

```elan
public action log_middleware {
    print req.method() + " " + req.path();
    // Middlewares run sequentially before the route handler
}

server.use("log_middleware");
```

### Static file serving

```elan
server.static("/public", "./www");
// GET /public/style.css  ->  ./www/style.css
```

### CORS

```elan
server.cors("*");                    // Allow all origins
server.cors("https://example.com");  // Allow specific origin
```

### Rate limiting

```elan
server.rate_limit("/api", "60", "60");  // 60 requests per 60 seconds on /api paths
```

### Route groups

```elan
string api = server.group("/api/v1");
api.get("/users", "users_list");
api.post("/users", "users_create");
```

### Logging

```elan
server.log_format("combined");
server.log_file("./access.log");
```

### Graceful shutdown

```elan
server.shutdown_graceful(5000);  // Shutdown with 5 second timeout
```

### WebSocket server

```elan
public action chat_handler {
    sock.send("Welcome to chat!");
    while (sock.state() == "open") {
        string msg = sock.recv_timeout("30000");
        if (msg != "") {
            sock.send("Server: " + msg);
        }
    }
}

server.ws("/chat", "chat_handler");
```

### Server-Sent Events (SSE)

```elan
public action sse_events {
    sse.emit("message", "Hello!");
    sse.emit("update", "{\"count\":42}");
    sse.close();
}

server.sse("/events", "sse_events");
```

### Complete backend example

```elan
@erelang
#include <builtin/network> as net

// Route handlers
public action home {
    res.html("<h1>Erelang Backend</h1>");
    res.end();
}

public action api_users_list {
    res.json("[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]");
    res.end();
}

public action api_users_create {
    string body = req.body();
    res.status("201");
    res.json("{\"status\":\"created\"}");
    res.end();
}

public action chat_handler {
    sock.send("Welcome to chat!");
    while (sock.state() == "open") {
        string msg = sock.recv_timeout("30000");
        if (msg != "") {
            sock.send("Server: " + msg);
        }
    }
}

// Middleware
public action access_log {
    print req.method() + " " + req.path();
}

public action main {
    string server = net.create_server(8080);

    // Middleware
    server.use("access_log");

    // Configuration
    server.cors("*");
    server.rate_limit("/api", "60", "60");
    server.static("/public", "./www");

    // Routes
    server.get("/", "home");
    server.get("/api/users", "api_users_list");
    server.post("/api/users", "api_users_create");

    // WebSocket
    server.ws("/chat", "chat_handler");

    // Logging
    server.log_format("combined");
    server.log_file("./access.log");

    // Start server
    server.listen();
}
```

## Filesystem (fs)

```elan
#include <builtin/fs> as fs
#include <builtin/path> as path

// File operations
bool exists = fs.exists("path/to/file");
string data = fs.read("path/to/file");
fs.write("path/to/file", "content");
fs.append("path/to/file", "more content");

// Directory operations — use list/dirs/files, not fs.read
fs.mkdir("./newdir");
Array<string> entries = fs.list("./dir");
for (string entry in entries) {
    if (fs.is_dir(entry)) { /* folder */ }
    if (fs.is_file(entry)) { /* regular file */ }
}
for (string folder in fs.dirs("commands")) {
    string name = path.name(folder);  // mod, dev, ...
}
for (string file in fs.files("./dir")) {
    print(file);
}
int bytes = fs.size("path/to/file");
int mtime = fs.mtime("path/to/file");

fs.remove("file.txt");
fs.copy("src.txt", "dst.txt");
fs.move("old.txt", "new.txt");
```

## Crypto

```elan
#include <builtin/crypto> as crypto

string hash = crypto.sha256("hello");
string md5 = crypto.md5("hello");
string hmac = crypto.hmac("sha256", "data", "secret");
string enc = crypto.encrypt("aes", "data", "key");
string dec = crypto.decrypt("aes", enc, "key");
string b64 = crypto.base64("hello");
string decoded = crypto.unbase64(b64);
string uuid = crypto.uuid();
string rand = crypto.random("32");
```

## Threads

```elan
#include <builtin/threads> as thread

thread.spawn("myTask");
thread.spawn_detached("background");
thread.sleep("1000");           // Sleep milliseconds
string result = thread.result("myTask");
thread.join("myTask");
thread.join_timeout("myTask", "5000");
thread.kill("myTask");
int active = thread.active();
thread.wait_all();
thread.done("myTask");
list tasks = thread.list();
thread.yield();
thread.gc();                    // GC completed threads
thread.gc_all();               // GC all threads
thread.remove("myTask");
string state = thread.state("myTask");
```

## Math

```elan
#include <builtin/math> as math

int sum = math.add(2, 3);
int diff = math.sub(10, 3);
int prod = math.mul(4, 5);
int quot = math.div(10, 2);
int mod = math.mod(10, 3);
int power = math.pow(2, 8);
double sqrt = math.sqrt(16);
double sin = math.sin(3.14);
double cos = math.cos(0);
int abs = math.abs(-5);
int min = math.min(3, 7);
int max = math.max(3, 7);
int clamp = math.clamp(15, 0, 10);  // 10
double random = math.random();
int randInt = math.random_int(1, 100);
```

## System (sys)

```elan
#include <builtin/system> as sys

sys.print("Hello");
sys.execute("dir");              // Execute with output capture
sys.cmd("echo Hello");            // Execute without capture
sys.output("command");            // Run and return output
string env = sys.env("PATH");
sys.set_env("KEY", "VALUE");
sys.sleep("1000");
string time = sys.now();
string cwd = sys.cwd();
string os = sys.os();            // "windows", "linux", etc.
string arch = sys.arch();        // "x64", "arm64", etc.
```

## Data

```elan
#include <builtin/data> as data

string json = data.to_json("{\"key\":\"val\"}");
// ... list/dict operations
```

## Binary

```elan
#include <builtin/binary> as bin

string buf = bin.new();          // Create empty buffer
string buf = bin.from_hex("deadbeef");
int len = bin.len(buf);
string hex = bin.to_hex(buf);
bin.push_u8(buf, 255);
int val = bin.get_u8(buf, 0);
```

## Regex

```elan
#include <builtin/regex> as regex

bool matches = regex.test(pattern, text);
list groups = regex.match(pattern, text);
string result = regex.replace(pattern, replacement, text);
list all = regex.match_all(pattern, text);
string escaped = regex.escape("hello.world");  // "hello\\.world"
```

## Monitor

```elan
#include <builtin/monitor> as monitor

string id = monitor.add("my_var", "1000");  // Monitor variable every 1000ms
monitor.remove(id);
list monitors = monitor.list();
string info = monitor.info(id);
string change = monitor.last_change(id);
monitor.set_interval(id, "500");
```

## Permissions

```elan
#include <builtin/perm> as perm

// Permission management
bool ok = perm.check("network");
perm.request("filesystem");
perm.revoke("network");
list perms = perm.list();
```

## Process

Protected builtins under `sys.*`:
```elan
sys.execute("command")     // Execute with protection
sys.cmd("command")         // Execute without output capture
sys.output("command")      // Run and capture output
```

## Compilation

```elan
// Compile to executable
erelang --compile app.elan                    // -> app.exe (no manifest)
erelang --compile app.elan --manifest         // -> app.exe + manifest.erelang
erelang --compile app.elan --lto --strip      // Optimized build
```

## Enums

```elan
enum Status {
    Pending,
    Active,
    Completed,
    Failed,
}

Status s = Status.Active;
if (s == Status.Completed) { ... }
```

## Generics

User-defined type parameters on structs, entities, enums, aliases, and actions. Bodies are checked **opaque**: operations on `T` require a trait constraint (not C++-style late validation).

```elan
struct Pair<A, B> {
    A first;
    B second;
}

enum Option<T> {
    Some(T),
    None
}

type StringMap<T> = Map<string, T>;

public action identity<T>(value: T): T {
    return value;
}

trait Comparable<T> {
    action compare(other: T): int;
}

public action max<T: Comparable<T>>(a: T, b: T): T {
    if (a.compare(b) > 0) { return a; }
    return b;
}

int x = identity(42);
Option<int> o = Option<int>.Some(1);
match (o) {
    case Some(v): { print v; }
    case None: { print "empty"; }
}
```

Constraints use `T: Trait` and `T: Trait & Other`. Instantiation is monomorphized under canonical names such as `identity<int>` and `struct:Pair<int, string>`.

## Match (enum patterns)

`match` destructures enum values. Patterns are variant constructors with nested bindings:

```elan
match (value) {
    case Some(v): { print v; }
    case None: { print "empty"; }
}

match (result) {
    case Ok(Some(n)): { print n; }
    case Ok(None): { print "empty"; }
    case Error(e): { print e; }
}

match (pair) {
    case Values(a, b): { print a; print b; }
}
```

- Scrutinee must be an enum (`Option`, `Result`, or user enum).
- Bindings are scoped to the case body; payload types use specialized type arguments.
- Nested variant patterns are allowed when a payload is an enum.
- Wildcard `_` is allowed in payload positions and as a catch-all case.
- No exhaustiveness checking, no guards, no `let Some(x) = ...` declaration destructuring, no struct field patterns.

See `examples/match.elan` and negative cases `examples/match_neg_*.elan`.

## Extern Declarations (C ABI)

```elan
extern pub fn MessageBoxA(hWnd: int, text: string, caption: string, flags: int): int
```

## Error Handling

Errors are reported via return values:
```elan
string data = net.get("https://api.example.com");
if (data == "") {
    print "request failed";
    return;
}
```

Type errors are reported at compile time when `@strict` is enabled.

## First-Class Functions (Lambdas)

Erelang supports first-class functions via the `lambda` keyword. Lambdas create closures that capture variables from the enclosing scope by value.

### Lambda Expressions

```elan
// Arrow form (single expression)
string doubler = lambda(x: int) -> x * 2;

// Block form (multiple statements)
string greet = lambda(name: string): string {
    string msg = "Hello, " + name + "!";
    return msg;
};
```

### Calling Lambdas

Lambdas return a `func:N` handle string. They can be called like any action:

```elan
string f = lambda(x: int) -> x + 1;
string r = f(41);  // r = "42"
```

### Higher-Order Functions

Built-in `map`, `filter`, `reduce` accept lambda handles:

```elan
string square = lambda(n: int) -> n * n;
string squares = map([1, 2, 3], square);    // map(list, func) -> new list

string isEven = lambda(x: int) -> x % 2;
string evens = filter([1,2,3,4], isEven);   // filter(list, func) -> new list

string sum = lambda(a: int, b: int) -> a + b;
string total = reduce([10,20,30], sum, 0);  // reduce(list, func, init) -> value
```

### Closure Capture

Variables from the enclosing scope are captured by value at creation time:

```elan
int multiplier = 3;
string scale = lambda(x: int) -> x * multiplier;  // captures multiplier = 3
string r = scale(10);  // r = "30"
```

## Related Documentation

| Topic | File |
|---|---|
| Language overview | [language.md](language.md) |
| Core builtins | [core-builtins.md](core-builtins.md) |
| Imports & modules | [imports.md](imports.md) |
| Network (HTTP/WS) | [network.md](network.md) |
| Filesystem | [filesystem.md](filesystem.md) |
| Collections | [collections.md](collections.md) |
| Threads & concurrency | [threads.md](threads.md) |
| Crypto | [crypto.md](crypto.md) |
| Math | [math.md](math.md) |
| Process management | [process.md](process.md) |
| Data operations | [data.md](data.md) |
| Binary operations | [binary.md](binary.md) |
| Regex | [regex.md](regex.md) |
| Monitor | [monitor.md](monitor.md) |
| Permissions | [permissions.md](permissions.md) |
| Toolchain | [toolchain.md](toolchain.md) |
| Diagnostics | [diagnostics.md](diagnostics.md) |
