#!/usr/bin/env python3
"""A stand-in for sc_http_get's test: listens on 127.0.0.1 (plain HTTP) on a port of the system's
choice and says which ("listening PORT"). GET /api/echo answers JSON of the query's parameters and
the X-Test header; /api/text answers plain text; /api/redirect a 302 to /api/echo."""
import http.server, json, sys, urllib.parse

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path == "/api/echo":
            body = json.dumps({"params": dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True)),
                               "header": self.headers.get("X-Test")}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        elif url.path == "/api/text":
            body = b"plain words"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
        elif url.path == "/api/redirect":
            body = b""
            self.send_response(302)
            self.send_header("Location", "/api/echo")
        else:
            body = b"no"
            self.send_response(500)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
print("listening", server.server_address[1], flush=True)
server.serve_forever()
