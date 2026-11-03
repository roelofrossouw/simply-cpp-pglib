"""A stand-in for 1web's /interface/index.php for sc_oneweb's tests.
Usage: oneweb_server.py SERVER_CERT SERVER_KEY KEY_FILE
Listens on 127.0.0.1 with HTTPS on a port of the system's choice and says which ("listening PORT").
Checks each request's signature as 1web does (the key from KEY_FILE, read on every request; a
nonce used once), answering 403 {"ok": false, "error": "Wrong signature"} otherwise. POST
/<site>/<name>, where site is main or branch:

  ping     {"ok": true, "data": {"site", "body" (as sent), "database", "role", "session_role", "agent", "type"}}
  refuse   {"ok": false, "error": "no such person"}
  broken   500, "Internal trouble"
  html     200, an HTML page
  big      200, {"ok": true, "data": "xxx..."}, 2000 bytes
  slow     the ping answer after 4 s
  moved    302 to /main/ping
  nothing  {"ok": true, "data": null}
  number   {"ok": true, "data": 12345678901234567890.123456789}
"""
import hashlib, hmac, http.server, json, re, ssl, sys, threading, time

server_cert, server_key, key_file = sys.argv[1:4]
seen = set()
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, body, content_type="application/json", extra=None):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in (extra or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def refuse(self, status, message):
        self.send(status, json.dumps({"ok": False, "error": message}))

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        match = re.fullmatch(r"/(main|branch)/([a-z][a-z0-9_]*)", self.path)
        if not match:
            return self.refuse(404, "No such call")
        site, name = match.groups()
        h = self.headers
        database, timestamp = h.get("X-OneWeb-Database", ""), h.get("X-OneWeb-Timestamp", "")
        nonce, signature = h.get("X-OneWeb-Nonce", ""), h.get("X-OneWeb-Signature", "").lower()
        if not timestamp.isdigit() or not re.fullmatch(r"[0-9a-f]{32}", nonce) or not database:
            return self.refuse(403, "Missing or malformed signature headers")
        if abs(time.time() - int(timestamp)) > 60:
            return self.refuse(403, "The request is too old")
        with open(key_file, "rb") as f:
            key = f.read().strip()
        message = b"\n".join([name.encode(), timestamp.encode(), nonce.encode(), database.encode(), body])
        if not hmac.compare_digest(hmac.new(key, message, hashlib.sha256).hexdigest(), signature):
            return self.refuse(403, "Wrong signature")
        with lock:
            if nonce in seen:
                return self.refuse(403, "The request was already used")
            seen.add(nonce)
        ping = {"ok": True, "data": {"site": site, "body": body.decode(), "database": database, "role": h.get("X-OneWeb-Role"),
                                     "session_role": h.get("X-OneWeb-Session-Role"), "agent": h.get("User-Agent"),
                                     "type": h.get("Content-Type")}}
        if name == "ping":
            self.send(200, json.dumps(ping))
        elif name == "refuse":
            self.refuse(200, "no such person")
        elif name == "broken":
            self.send(500, "Internal trouble", "text/plain")
        elif name == "html":
            self.send(200, "<html><body>not json</body></html>", "text/html")
        elif name == "big":
            self.send(200, json.dumps({"ok": True, "data": "x" * 1980}))
        elif name == "slow":
            time.sleep(4)
            self.send(200, json.dumps(ping))
        elif name == "moved":
            self.send(302, "", "text/plain", {"Location": "/main/ping"})
        elif name == "nothing":
            self.send(200, '{"ok": true, "data": null}')
        elif name == "number":
            self.send(200, '{"ok": true, "data": 12345678901234567890.123456789}')
        else:
            self.refuse(404, "No such call")


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(server_cert, server_key)
server.socket = context.wrap_socket(server.socket, server_side=True)
print("listening", server.server_address[1], flush=True)
server.serve_forever()
