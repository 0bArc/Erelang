"""Mock Discord server for testing plugins/discord without a real bot token.

Serves the REST API (http) and the gateway (ws) on localhost and records every request
and every gateway frame it receives as JSON lines, so a test can assert on them.

    python mock_discord.py --rest-port 18110 --ws-port 18111 --log run.jsonl

Standard library only. Scenario (what the gateway does after a client connects):

  1. HELLO (op 10) with a short heartbeat interval.
  2. On IDENTIFY (op 2): READY, then MESSAGE_CREATE and INTERACTION_CREATE events, then
     RECONNECT (op 7) and close the socket.
  3. On RESUME (op 6): RESUMED, then after 0.4s one more MESSAGE_CREATE ("!after-resume").
  4. Every HEARTBEAT (op 1) is answered with HEARTBEAT_ACK (op 11).

REST: the first POST to /channels/c1/messages answers 429 with retry_after so a client's
rate-limit retry is exercised; everything else answers 200 with a small JSON body.
"""
import argparse
import base64
import hashlib
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
HEARTBEAT_MS = 250

args = None
log_lock = threading.Lock()
state = {"rate_limited_once": False, "identifies": 0, "resumes": 0}


def log(kind, **fields):
    entry = {"kind": kind, "t": round(time.time(), 3), **fields}
    with log_lock:
        with open(args.log, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")


# ---------------------------------------------------------------- REST
class Rest(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n).decode("utf-8") if n else ""

    def _reply(self, status, obj, extra=None):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _handle(self, method):
        body = self._body()
        log("rest", method=method, path=self.path,
            auth=self.headers.get("Authorization", ""),
            ctype=self.headers.get("Content-Type", ""), body=body)
        path = self.path.split("?")[0]
        if path == "/api/v10/gateway":
            return self._reply(200, {"url": "ws://127.0.0.1:%d" % args.ws_port})
        if path == "/api/v10/gateway/bot":
            return self._reply(200, {"url": "ws://127.0.0.1:%d" % args.ws_port, "shards": 1})
        if method == "POST" and path == "/api/v10/channels/c1/messages" and not state["rate_limited_once"]:
            state["rate_limited_once"] = True
            return self._reply(429, {"message": "You are being rate limited.",
                                     "retry_after": 0.2, "global": False},
                               {"Retry-After": "1"})
        if self.headers.get("Authorization", "") == "" and "/interactions/" not in path:
            return self._reply(401, {"message": "401: Unauthorized", "code": 0})
        if path.endswith("/channels/missing/messages"):
            return self._reply(404, {"message": "Unknown Channel", "code": 10003})
        try:
            parsed = json.loads(body) if body else {}
        except ValueError:
            return self._reply(400, {"message": "Invalid JSON", "code": 50109})
        out = {"id": "m1", "ok": True}
        if isinstance(parsed, dict):
            out["echo_content"] = parsed.get("content", "")
        return self._reply(200, out)

    def do_GET(self): self._handle("GET")
    def do_POST(self): self._handle("POST")
    def do_PATCH(self): self._handle("PATCH")
    def do_PUT(self): self._handle("PUT")
    def do_DELETE(self): self._handle("DELETE")


# ---------------------------------------------------------------- WebSocket
def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def read_frame(sock):
    b1, b2 = recv_exact(sock, 2)
    opcode = b1 & 0x0F
    masked = b2 & 0x80
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else b""
    data = recv_exact(sock, length) if length else b""
    if masked:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data


def send_text(sock, text):
    data = text.encode()
    header = bytes([0x81])
    n = len(data)
    if n < 126:
        header += bytes([n])
    elif n < 65536:
        header += bytes([126]) + struct.pack(">H", n)
    else:
        header += bytes([127]) + struct.pack(">Q", n)
    sock.sendall(header + data)


def send_close(sock, code):
    try:
        sock.sendall(bytes([0x88, 2]) + struct.pack(">H", code))
    except OSError:
        pass


def dispatch(sock, seq, name, data):
    send_text(sock, json.dumps({"op": 0, "s": seq, "t": name, "d": data}))


def gateway_client(sock):
    seq = [0]
    lock = threading.Lock()

    def send_event(name, data):
        with lock:
            seq[0] += 1
            dispatch(sock, seq[0], name, data)

    send_text(sock, json.dumps({"op": 10, "d": {"heartbeat_interval": HEARTBEAT_MS}}))
    log("gw", event="hello_sent")
    try:
        while True:
            opcode, data = read_frame(sock)
            if opcode == 8:
                log("gw", event="client_close", code=struct.unpack(">H", data[:2])[0] if len(data) >= 2 else 0)
                return
            if opcode == 9:
                sock.sendall(bytes([0x8A, len(data)]) + data)
                continue
            if opcode != 1:
                continue
            text = data.decode("utf-8", "replace")
            frame = json.loads(text)
            op = frame.get("op")
            log("gw", event="frame", op=op, raw=text)
            if op == 1:
                with lock:
                    send_text(sock, json.dumps({"op": 11}))
            elif op == 2:
                state["identifies"] += 1
                send_event("READY", {"v": 10, "session_id": "sess1",
                                     "resume_gateway_url": "ws://127.0.0.1:%d" % args.ws_port,
                                     "user": {"id": "bot1", "username": "mockbot"},
                                     "application": {"id": "app1"}})
                time.sleep(0.15)
                send_event("MESSAGE_CREATE", {"id": "msg1", "channel_id": "c1", "content": "!ping",
                                              "author": {"id": "u1", "username": "alice", "bot": False}})
                send_event("MESSAGE_CREATE", {"id": "msg2", "channel_id": "c1", "content": "bot talking",
                                              "author": {"id": "bot1", "username": "mockbot", "bot": True}})
                send_event("INTERACTION_CREATE", {"id": "i1", "token": "tok1", "type": 2,
                                                  "channel_id": "c1",
                                                  "data": {"name": "echo",
                                                           "options": [{"name": "message", "type": 3,
                                                                        "value": "hi \"there\""}]},
                                                  "member": {"user": {"id": "u1", "username": "alice"}}})
                time.sleep(0.6)
                with lock:
                    send_text(sock, json.dumps({"op": 7, "d": None}))
                log("gw", event="reconnect_sent")
                time.sleep(0.1)
                return
            elif op == 6:
                state["resumes"] += 1
                with lock:
                    seq[0] = int(frame["d"].get("seq") or 0)
                send_event("RESUMED", {})
                time.sleep(0.4)  # long enough for the client to heartbeat before it is told to stop
                send_event("MESSAGE_CREATE", {"id": "msg3", "channel_id": "c1", "content": "!after-resume",
                                              "author": {"id": "u1", "username": "alice", "bot": False}})
    except (ConnectionError, OSError):
        log("gw", event="connection_lost")
    finally:
        try:
            sock.close()
        except OSError:
            pass


def ws_accept(conn):
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = conn.recv(4096)
        if not chunk:
            return
        request += chunk
    headers = {}
    for line in request.decode("latin-1").split("\r\n")[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    key = headers.get("sec-websocket-key", "")
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
    log("gw", event="connected", path=request.split(b" ")[1].decode("latin-1"))
    gateway_client(conn)


def ws_server():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.ws_port))
    srv.listen(8)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=ws_accept, args=(conn,), daemon=True).start()


def main():
    global args
    p = argparse.ArgumentParser()
    p.add_argument("--rest-port", type=int, default=18110)
    p.add_argument("--ws-port", type=int, default=18111)
    p.add_argument("--log", default="mock_discord.jsonl")
    args = p.parse_args()
    open(args.log, "w").close()
    threading.Thread(target=ws_server, daemon=True).start()
    ThreadingHTTPServer(("127.0.0.1", args.rest_port), Rest).serve_forever()


if __name__ == "__main__":
    main()
