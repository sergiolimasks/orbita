"""Lista de espera pública (pedido 2026-10-28).

Cadastro em /lista sem convite: a conta nasce com wait='waiting' e só vê o
aviso da lista. O admin libera N por sorteio (ordem de chegada não conta).
Liberado (wait='released') tem DEADLINE_H horas, a contar da liberação, pra
usar a chave num console; quando usa, vira testador normal (wait=''). Quem
passa do prazo sai de vez (wait='expired', conta desativada, chaves
revogadas) e a vaga não passa pra ninguém. Nada é notificado: a pessoa vê
o estado entrando no site.
"""
import re

from . import db

DEADLINE_H = 48
RELEASE_MAX = 500
PER_IP_DAY = 3   # cadastros na lista por IP em 24 h

REDDIT_RE = re.compile(r"^[A-Za-z0-9_-]{3,20}$")


def clean_reddit(s):
    """'u/Fulano', '/u/Fulano', '@Fulano' ou link do perfil -> 'Fulano'."""
    s = (s or "").strip()
    m = re.search(r"reddit\.com/(?:u|user)/([^/?#\s]+)", s, re.I)
    if m:
        s = m.group(1)
    s = s.lstrip("@")
    for p in ("/u/", "u/"):
        if s.lower().startswith(p):
            s = s[len(p):]
            break
    return s.strip("/ ")


def reddit_taken(name):
    return bool(db.row("SELECT 1 FROM users WHERE lower(reddit)=lower(%s)", name))


def ip_full(ip):
    r = db.row("SELECT count(*) AS n FROM login_attempts WHERE ip=%s AND kind='waitlist' AND ok "
               "AND at > now() - interval '1 day'", ip)
    return r["n"] >= PER_IP_DAY


def release(n):
    """Sorteia até n da lista e libera. Devolve os ids liberados."""
    n = max(0, min(int(n), RELEASE_MAX))
    if not n:
        return []
    with db.conn() as c:
        rows = c.execute("UPDATE users SET wait='released', released_at=now() WHERE id IN ("
                         "SELECT id FROM users WHERE wait='waiting' AND active ORDER BY random() LIMIT %s "
                         "FOR UPDATE SKIP LOCKED) RETURNING id", (n,)).fetchall()
    return [r["id"] for r in rows]


def expire():
    """Idempotente (roda em qualquer worker): quem já usou a chave num console
    vira testador normal; quem passou do prazo sai. Devolve os ids que saíram."""
    with db.conn() as c:
        c.execute("UPDATE users SET wait='' WHERE wait='released' AND EXISTS ("
                  "SELECT 1 FROM beta_keys k WHERE k.user_id=users.id AND k.bound_at IS NOT NULL)")
        ids = [r["id"] for r in c.execute(
            "UPDATE users SET wait='expired', active=false WHERE wait='released' "
            "AND released_at < now() - make_interval(hours => %s) RETURNING id", (DEADLINE_H,)).fetchall()]
        if ids:
            c.execute("UPDATE beta_keys SET revoked_at=now(), pending_enc=NULL "
                      "WHERE user_id = ANY(%s) AND revoked_at IS NULL", (ids,))
            c.execute("DELETE FROM sessions WHERE user_id = ANY(%s)", (ids,))
    return ids


def counts():
    return db.row("SELECT count(*) FILTER (WHERE wait='waiting' AND active) AS waiting, "
                  "count(*) FILTER (WHERE wait='released') AS released, "
                  "count(*) FILTER (WHERE wait='expired') AS expired FROM users")
