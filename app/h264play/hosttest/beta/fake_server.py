#!/usr/bin/env python3
"""XCloud360 / orbita v0.20.3 — ponte FAKE local (HTTP puro) com o contrato do
GET /v1/beta/check, pros testes do host (hosttest/beta/run_beta.sh fake).

  204                       chave válida, console = o vinculado (1o uso vincula)
  401 {"reason":"invalid"}  chave desconhecida
  403 {"reason":"revoked"}  chave revogada
  403 {"reason":"duplicate"} chave usada em 2 consoles: derrubada pros dois
  400 {"reason":"console"}  X-Console ausente ou fora de 32 hex minúsculos
  503 {"reason":"unavailable"} (chave UNAVL-KEY23: backend fora)
A ponte compara a chave sem '-'/espaços e sem caixa.
Uso: fake_server.py <porta>
"""
import json, re, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

KEYS = {"GOODKEY234": "ok", "REVKDKEY23": "revoked", "NEWKY23456": "ok", "UNAVLKEY23": "down"}
BOUND, DEAD = {}, set()
CON_RE = re.compile(r"^[0-9a-f]{32}$")


def canon(k):
    return re.sub(r"[-\s]", "", k or "").upper()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, reason=None):
        body = json.dumps({"reason": reason}).encode() if reason else b""
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        if body:
            self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path != "/v1/beta/check":
            return self.send(404)
        k = canon(self.headers.get("X-Api-Key"))
        con = self.headers.get("X-Console") or ""
        if not CON_RE.match(con):
            return self.send(400, "console")
        st = KEYS.get(k)
        if st is None:
            return self.send(401, "invalid")
        if st == "revoked":
            return self.send(403, "revoked")
        if st == "down":
            return self.send(503, "unavailable")
        if k in DEAD:
            return self.send(403, "duplicate")
        if BOUND.setdefault(k, con) != con:
            DEAD.add(k)
            return self.send(403, "duplicate")
        return self.send(204)


if __name__ == "__main__":
    srv = HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
    print("fake ponte em 127.0.0.1:%s" % sys.argv[1], flush=True)
    srv.serve_forever()
