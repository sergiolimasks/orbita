"""Chaves beta: criação/revogação e a regra CHAVE = UM CONSOLE.

O Postgres do site é a fonte da verdade. A ponte (xc360-logs) não lê arquivo
nem banco: a cada pedido com chave (com cache curto do lado dela) chama
POST /_bridge/check aqui (rede interna, segredo compartilhado; ver
views/bridge.py), mandando sha256 da chave normalizada, o X-Console do Xbox e
um hash do IP. `check()` decide e grava o vínculo numa transação só:

  ok         chave ativa; se ainda não tinha console, fica presa a este
  invalid    hash desconhecido
  revoked    revogada pelo admin / trocada / testador desativado
  duplicate  um 2º console diferente usou a chave: ela cai na hora (dup_at)
             e os dois consoles passam a receber 403 até o admin agir

Só o admin libera uma chave derrubada: `unbind()` (troca legítima de console;
a mesma chave volta a valer pro próximo console) ou `new_key(by_admin=...)`
(chave nova, que o testador vê uma vez na área dele). A chave em claro nunca
fica no banco; a gerada pelo admin fica só cifrada (Fernet) em pending_enc até
o testador abrir a área.
"""
import re, secrets

from . import db
from .security import KEY_ALPHA, dec, enc, key_hash

CONSOLE_RE = re.compile(r"^[0-9a-f]{32}$")
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
IPH_RE = re.compile(r"^[0-9a-f]{0,64}$")


def _gen():
    while True:
        s = "".join(secrets.choice(KEY_ALPHA) for _ in range(10))
        k = s[:5] + "-" + s[5:]
        if not db.row("SELECT 1 FROM beta_keys WHERE key_sha256=%s", key_hash(k)):
            return k


def new_key(user, conn=None, by_admin=None):
    """Revoga a(s) chave(s) ativa(s) do usuário e cria outra (sem console).
    Devolve a chave em claro — mostrada UMA vez. by_admin=<admin>: guarda a
    chave cifrada pro testador ver uma vez na área dele."""
    k = _gen()

    def work(c):
        old = c.execute("UPDATE beta_keys SET revoked_at=now(), pending_enc=NULL, key_enc=NULL WHERE user_id=%s AND revoked_at IS NULL "
                        "RETURNING id, dup_at", (user["id"],)).fetchall()
        new = c.execute("INSERT INTO beta_keys(user_id, key_sha256, key_hint, pending_enc, key_enc) "
                        "VALUES (%s,%s,%s,%s,%s) RETURNING id",
                        (user["id"], key_hash(k), k[-2:], enc(k) if by_admin else None, enc(k))).fetchone()
        if by_admin:
            c.execute("INSERT INTO key_events(key_id, user_id, kind, by_user, note) VALUES (%s,%s,'new_by_admin',%s,%s)",
                      (new["id"], user["id"], by_admin["id"],
                       "after_duplicate" if any(o["dup_at"] for o in old) else ""))

    if conn is not None:
        work(conn)
    else:
        with db.conn() as c:
            work(c)
    return k


def revoke_user_keys(user_id):
    return len(db.rows("UPDATE beta_keys SET revoked_at=now(), pending_enc=NULL, key_enc=NULL WHERE user_id=%s AND revoked_at IS NULL "
                       "RETURNING id", user_id))


def current(user_id):
    """Chave vigente (não revogada) do usuário, com o estado do vínculo."""
    return db.row("SELECT id, key_hint, created_at, console_id, bound_at, last_used_at, dup_at, dup_console, dup_ip_h, "
                  "pending_enc IS NOT NULL AS pending FROM beta_keys WHERE user_id=%s AND revoked_at IS NULL "
                  "ORDER BY id DESC LIMIT 1", user_id)


def reveal(user_id):
    """v0.21.38: a chave vigente em claro (só pro dono, na área dele). None = chave antiga, sem cópia."""
    r = db.row("SELECT key_enc FROM beta_keys WHERE user_id=%s AND revoked_at IS NULL AND key_enc IS NOT NULL "
               "ORDER BY id DESC LIMIT 1", user_id)
    if not r:
        return None
    try:
        return dec(r["key_enc"])
    except Exception:
        return None


def take_pending(user_id):
    """Chave gerada pelo admin, em claro, uma vez só (apaga a cópia cifrada)."""
    with db.conn() as c:
        r = c.execute("SELECT id, pending_enc FROM beta_keys WHERE user_id=%s AND revoked_at IS NULL "
                      "AND pending_enc IS NOT NULL FOR UPDATE", (user_id,)).fetchone()
        if not r:
            return None
        c.execute("UPDATE beta_keys SET pending_enc=NULL WHERE id=%s", (r["id"],))
    return dec(r["pending_enc"])


def is_blocked(user_id):
    k = current(user_id)
    return bool(k and k["dup_at"])


def unbind(user_id, admin):
    """Troca legítima de console: a chave vigente continua e vale pro próximo console."""
    with db.conn() as c:
        k = c.execute("SELECT id, console_id, dup_console FROM beta_keys WHERE user_id=%s AND revoked_at IS NULL "
                      "ORDER BY id DESC LIMIT 1 FOR UPDATE", (user_id,)).fetchone()
        if not k:
            return False
        c.execute("UPDATE beta_keys SET console_id=NULL, bound_at=NULL, dup_at=NULL, dup_console=NULL, dup_ip_h=NULL "
                  "WHERE id=%s", (k["id"],))
        c.execute("INSERT INTO key_events(key_id, user_id, kind, console_a, console_b, by_user) "
                  "VALUES (%s,%s,'unbind',%s,%s,%s)",
                  (k["id"], user_id, k["console_id"] or "", k["dup_console"] or "", admin["id"]))
    return True


XEX_IP_IDLE = "1 hour"   # IP novo assume se o antigo ficou parado esse tempo


def check(kh, console, ip_h="", bind="hw"):
    """Validação + vínculo atômicos. Devolve (status, handle|None).
    bind="hw" (ELF): a chave fica presa ao 1º console (console_id).
    bind="ip" (XEX, 2026-10-29): presa ao IP público (ip_h), à parte do console_id; IP novo assume
    se o antigo ficou XEX_IP_IDLE sem uso; dois IPs ao mesmo tempo = duplicada."""
    if not HASH_RE.match(kh or "") or not CONSOLE_RE.match(console or ""):
        return "invalid", None
    ip_h = ip_h if IPH_RE.match(ip_h or "") else ""
    if bind == "ip":
        return _check_ip(kh, console, ip_h)
    with db.conn() as c:
        k = c.execute("SELECT k.id, k.user_id, k.revoked_at, k.console_id, k.dup_at, k.last_used_at > now() - interval '5 minutes' AS fresh, "
                      "u.active, u.handle FROM beta_keys k JOIN users u ON u.id=k.user_id "
                      "WHERE k.key_sha256=%s FOR UPDATE OF k", (kh,)).fetchone()
        if not k:
            return "invalid", None
        if k["revoked_at"] is not None or not k["active"]:
            return "revoked", k["handle"]
        if k["dup_at"] is not None:
            return "duplicate", k["handle"]
        if k["console_id"] is None:
            c.execute("UPDATE beta_keys SET console_id=%s, bound_at=now(), last_used_at=now() WHERE id=%s", (console, k["id"]))
            c.execute("INSERT INTO key_events(key_id, user_id, kind, console_a, ip_h) VALUES (%s,%s,'bind',%s,%s)",
                      (k["id"], k["user_id"], console, ip_h))
            return "ok", k["handle"]
        if k["console_id"] != console:
            c.execute("UPDATE beta_keys SET dup_at=now(), dup_console=%s, dup_ip_h=%s WHERE id=%s", (console, ip_h, k["id"]))
            c.execute("INSERT INTO key_events(key_id, user_id, kind, console_a, console_b, ip_h) "
                      "VALUES (%s,%s,'duplicate',%s,%s,%s)", (k["id"], k["user_id"], k["console_id"], console, ip_h))
            return "duplicate", k["handle"]
        if not k["fresh"]:
            c.execute("UPDATE beta_keys SET last_used_at=now() WHERE id=%s", (k["id"],))
        return "ok", k["handle"]


def _check_ip(kh, console, ip_h):
    if not ip_h:
        return "invalid", None
    with db.conn() as c:
        k = c.execute("SELECT k.id, k.user_id, k.revoked_at, k.dup_at, k.xex_ip_h, "
                      "k.xex_last_at > now() - interval '5 minutes' AS fresh, "
                      "(k.xex_last_at IS NULL OR k.xex_last_at < now() - interval '" + XEX_IP_IDLE + "') AS idle, "
                      "u.active, u.handle FROM beta_keys k JOIN users u ON u.id=k.user_id "
                      "WHERE k.key_sha256=%s FOR UPDATE OF k", (kh,)).fetchone()
        if not k:
            return "invalid", None
        if k["revoked_at"] is not None or not k["active"]:
            return "revoked", k["handle"]
        if k["dup_at"] is not None:
            return "duplicate", k["handle"]
        if k["xex_ip_h"] is None or (k["xex_ip_h"] != ip_h and k["idle"]):
            kind = "bind_xex" if k["xex_ip_h"] is None else "xex_ip_change"
            c.execute("UPDATE beta_keys SET xex_ip_h=%s, xex_last_at=now(), last_used_at=now() WHERE id=%s", (ip_h, k["id"]))
            c.execute("INSERT INTO key_events(key_id, user_id, kind, console_a, ip_h) VALUES (%s,%s,%s,%s,%s)",
                      (k["id"], k["user_id"], kind, console, ip_h))
            return "ok", k["handle"]
        if k["xex_ip_h"] != ip_h:
            c.execute("UPDATE beta_keys SET dup_at=now(), dup_console=%s, dup_ip_h=%s WHERE id=%s", (console, ip_h, k["id"]))
            c.execute("INSERT INTO key_events(key_id, user_id, kind, console_a, console_b, ip_h) "
                      "VALUES (%s,%s,'duplicate_ip',%s,%s,%s)", (k["id"], k["user_id"], k["xex_ip_h"], ip_h, ip_h))
            return "duplicate", k["handle"]
        if not k["fresh"]:
            c.execute("UPDATE beta_keys SET xex_last_at=now(), last_used_at=now() WHERE id=%s", (k["id"],))
        return "ok", k["handle"]


def blocked_count():
    return db.row("SELECT count(*) AS n FROM beta_keys k JOIN users u ON u.id=k.user_id "
                  "WHERE k.revoked_at IS NULL AND k.dup_at IS NOT NULL AND u.active")["n"]
