#!/usr/bin/env python3
"""Local test server for BlackHole: HTTP + HTTPS, gzip, chunked, redirects.

usage: server.py HTTP_PORT HTTPS_PORT CERT KEY [DOCROOT]
"""
import gzip
import http.server
import os
import ssl
import sys
import threading


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    docroot = "."
    http_port = 0

    def log_message(self, *a):
        pass

    def send_body(self, body, ctype="text/html; charset=utf-8", extra=None, chunked=False):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for i in range(0, len(body), 7):
                piece = body[i:i + 7]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/hello":
            return self.send_body(b"<h1>hello from test server</h1>")
        if p == "/gzip":
            return self.send_body(gzip.compress(b"<p>gzip works</p>" * 50), extra={"Content-Encoding": "gzip"})
        if p == "/chunked":
            return self.send_body(b"<p>chunked works, with several small pieces</p>", chunked=True)
        if p == "/bomb":
            return self.send_body(gzip.compress(b"\0" * (200 * 1024 * 1024)), "application/octet-stream",
                                  {"Content-Encoding": "gzip"})
        if p in ("/trunc", "/trunc-empty", "/trunc-gz"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            if p == "/trunc-gz":
                self.send_header("Content-Encoding", "gzip")
            self.end_headers()
            if p == "/trunc":
                for piece in (b"<h1>parcial</h1>", b"<p>chegou ate aqui</p>"):
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            elif p == "/trunc-gz":
                z = gzip.compress(b"<h1>gzip parcial</h1>" + b"<p>linha de texto</p>" * 200)
                piece = z[: len(z) // 2]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.flush()
            self.close_connection = True
            return
        if p == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/hello")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        if p == "/loop":
            self.send_response(302)
            self.send_header("Location", "/loop")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if p == "/downgrade":
            self.send_response(302)
            self.send_header("Location", "http://localhost:%d/hello" % H.http_port)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        # static files from docroot
        fp = os.path.normpath(os.path.join(H.docroot, p.lstrip("/")))
        if not fp.startswith(os.path.abspath(H.docroot)):
            self.send_error(403)
            return
        if os.path.isdir(fp):
            fp = os.path.join(fp, "index.html")
        if os.path.isfile(fp):
            ext = os.path.splitext(fp)[1]
            ct = {".html": "text/html; charset=utf-8", ".css": "text/css", ".png": "image/png",
                  ".jpg": "image/jpeg", ".txt": "text/plain"}.get(ext, "application/octet-stream")
            with open(fp, "rb") as f:
                return self.send_body(f.read(), ct)
        self.send_response(404)
        self.send_header("Content-Length", "9")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b"not found")
        self.close_connection = True


def main():
    http_port, https_port, cert, key = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    H.docroot = os.path.abspath(sys.argv[5]) if len(sys.argv) > 5 else os.path.abspath(".")
    H.http_port = http_port
    s1 = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), H)
    s2 = http.server.ThreadingHTTPServer(("127.0.0.1", https_port), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    s2.socket = ctx.wrap_socket(s2.socket, server_side=True)
    threading.Thread(target=s1.serve_forever, daemon=True).start()
    print("ready", flush=True)
    s2.serve_forever()


if __name__ == "__main__":
    main()
