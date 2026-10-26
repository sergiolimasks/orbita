#!/usr/bin/env python3
"""XCloud360 — conferir uma chave beta na ponte (x360.pistatrack.com).

As chaves NÃO são mais geridas aqui: o site da beta (web/, Postgres) é a fonte
da verdade — criar, revogar, gerar nova e desvincular console é no painel admin
(/admin/testers). Regra: CHAVE = UM CONSOLE (vincula no 1º uso; um 2º console
derruba a chave e só o admin libera). O keys.tsv da ponte foi aposentado.
A chave de desenvolvimento (~/credentials/xc360-logs-key, segredo docker
xc360_logs_key) continua valendo na ponte, sem vínculo de console.

  beta_keys.py check <chave> [--console HEX32]   GET /v1/beta/check
  beta_keys.py check --dev [--console HEX32]     idem com a chave dev

ATENÇÃO: `check` com chave de testador e --console VINCULA a chave a esse
console (ou a derruba, se ela já estiver presa a outro). Pra testar sem
efeito colateral use a chave dev.
"""
import argparse, json, os, re, sys, urllib.request, urllib.error

DEV_KEY = os.path.expanduser("~/credentials/xc360-logs-key")
URL = "https://x360.pistatrack.com"


def check(key, console=None, url=URL):
    h = {"X-Api-Key": key}
    if console:
        h["X-Console"] = console
    req = urllib.request.Request(url + "/v1/beta/check", headers=h)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, None
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}").get("reason")
        except ValueError:
            return e.code, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["check", "new", "list", "revoke", "push"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--dev", action="store_true")
    ap.add_argument("--console")
    ap.add_argument("--url", default=URL)
    a = ap.parse_args()
    if a.cmd != "check":
        sys.exit("'%s' foi aposentado: as chaves beta são geridas no site (%s/admin/testers)." % (a.cmd, a.url))
    key = open(DEV_KEY).read().strip() if a.dev else a.arg
    if not key:
        sys.exit("uso: check <chave> [--console HEX32] | check --dev")
    if a.console and not re.fullmatch(r"[0-9a-fA-F]{32}", a.console):
        sys.exit("--console: 32 caracteres hex")
    st, reason = check(key, a.console, a.url)
    print(st, "valida" if st == 204 else (reason or "invalida"))
    sys.exit(0 if st == 204 else 1)


if __name__ == "__main__":
    main()
