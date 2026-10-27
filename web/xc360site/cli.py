"""Comandos de manutenção (rodar dentro do container do site):

  python -m xc360site.cli admin-link [--hours 72]
      gera um LINK de configuração de administrador, de uso único. Quem abrir
      define nome, e-mail, senha e o 2FA (TOTP, obrigatório).
  python -m xc360site.cli sync-logs     indexa agora os lotes de log do R2
  python -m xc360site.cli backfill-plays
      (re)lê todos os lotes que ainda não passaram pelo parser de sessões de jogo
      (página /jogos). O sync normal já faz isso aos poucos; isto faz de uma vez.
"""
import argparse, sys

from . import config, db, logindex, security
from .storage import R2Storage


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("admin-link")
    p.add_argument("--hours", type=int, default=None)
    sub.add_parser("sync-logs")
    sub.add_parser("backfill-plays")
    a = ap.parse_args(argv)
    cfg = config.load()
    db.init(cfg["DATABASE_URL"], max_size=2)
    db.apply_schema()
    if a.cmd == "admin-link":
        hours = a.hours or cfg["SETUP_LINK_HOURS"]
        if not 1 <= hours <= 15 * 24:
            sys.exit("--hours entre 1 e 360")
        tok = security.token()
        db.run("INSERT INTO setup_links(token_sha256, expires_at) VALUES (%s, now() + make_interval(hours => %s))",
               security.sha256(tok), hours)
        print("%s/setup/%s" % (cfg["BASE_URL"], tok))
        print("(uso único, expira em %d h)" % hours, file=sys.stderr)
    elif a.cmd == "sync-logs":
        print(logindex.sync(R2Storage.from_env_text(cfg["R2_ENV"]), max_fetch=5000))
    elif a.cmd == "backfill-plays":
        print(logindex.backfill_all(R2Storage.from_env_text(cfg["R2_ENV"])))


if __name__ == "__main__":
    main()
