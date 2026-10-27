#!/usr/bin/env python3
"""Versão de teste só para um testador (aparece na área dele no site).

  tester_build.py put --user 35 --elf variants/v02131-volume.elf32 --version 0.21.31-volume --note "..."
  tester_build.py rm  --user 35

Grava no R2 tester-builds/<id>/xenon.elf e meta.json (o site lê em views/tester.py).
O app não recebe isso por atualização: o testador baixa e troca o xenon.elf na mão.
"""
import argparse, hashlib, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release  # noqa: E402  (cliente R2 e checagem do ELF)

KEY = "tester-builds/%d/%s"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("put")
    p.add_argument("--user", type=int, required=True)
    p.add_argument("--elf", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--note", default="")
    p = sub.add_parser("rm")
    p.add_argument("--user", type=int, required=True)
    a = ap.parse_args()
    r2 = release.R2()
    if a.cmd == "put":
        data = open(a.elf, "rb").read()
        release.check_elf(data)
        meta = {"version": a.version, "note": a.note.strip()[:400], "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}
        r2.put(KEY % (a.user, "xenon.elf"), data, "application/octet-stream")
        r2.put(KEY % (a.user, "meta.json"), json.dumps(meta, ensure_ascii=False).encode(), "application/json")
        print("testador %d: %s (%s…)" % (a.user, a.version, meta["sha256"][:8]))
    else:
        for f in ("meta.json", "xenon.elf"):
            print(f, r2.rm(KEY % (a.user, f)))


if __name__ == "__main__":
    main()
