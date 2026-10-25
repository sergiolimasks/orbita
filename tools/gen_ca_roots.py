#!/usr/bin/env python3
"""Gera app/h264play/source/login/ca_roots.c com as raízes CA embutidas.

Uso: gen_ca_roots.py <saida.c> <pem>...
Cadeias medidas em 2026-10-24 (openssl s_client -showcerts) para
login.microsoftonline.com, user.auth/xsts.auth.xboxlive.com,
xgpuweb(.f2p).gssv-play-prod.xboxlive.com, brs.core.gssv-play-prod e
displaycatalog.mp.microsoft.com: TODAS terminam em DigiCert Global Root G2.
As outras raízes da Microsoft/DigiCert entram de reserva (troca de CA).
"""
import subprocess
import sys

out, pems = sys.argv[1], sys.argv[2:]
parts = []
for p in pems:
    pem = open(p).read().strip() + "\n"
    info = subprocess.run(["openssl", "x509", "-noout", "-subject", "-enddate", "-fingerprint", "-sha256"],
                          input=pem, capture_output=True, text=True).stdout.strip().replace("\n", " | ")
    lines = "".join('"%s\\n"\n' % l for l in pem.splitlines())
    parts.append("/* %s */\n%s" % (info.replace("*/", "*_/"), lines))
src = ("/* AUTO-GERADO por tools/gen_ca_roots.py — raízes CA do XCloud360 v0.17. */\n"
       "const char xc_ca_roots_pem[] =\n" + "".join(parts) + ";\n")
open(out, "w").write(src)
print("wrote", out, len(pems), "roots")
