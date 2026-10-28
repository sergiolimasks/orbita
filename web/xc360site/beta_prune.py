"""Beta só pra quem testa (pedido 2026-10-28).

- disable(ids, reason): desativa a conta, revoga as chaves (o app passa a dar a
  chave como inválida) e derruba as sessões. O motivo vira a mensagem do login.
- prune_inactive(): automático. Quem entrou na beta há INACTIVE_DAYS dias, nunca
  usou uma chave num console e não mandou relato nenhum sai sem aviso.
"""
from . import db

INACTIVE_DAYS = 7
REASONS = ("noreport", "inactive", "launch")


def disable(ids, reason):
    assert reason in REASONS
    ids = [int(i) for i in ids]
    if not ids:
        return []
    with db.conn() as c:
        out = [r["id"] for r in c.execute(
            "UPDATE users SET active=false, disabled_reason=%s WHERE id = ANY(%s) AND role='tester' RETURNING id",
            (reason, ids)).fetchall()]
        if out:
            c.execute("UPDATE beta_keys SET revoked_at=now(), pending_enc=NULL, key_enc=NULL "
                      "WHERE user_id = ANY(%s) AND revoked_at IS NULL", (out,))
            c.execute("DELETE FROM sessions WHERE user_id = ANY(%s)", (out,))
    return out


def inactive_ids():
    return [r["id"] for r in db.rows(
        "SELECT u.id FROM users u WHERE u.role='tester' AND u.active AND u.wait='' "
        "AND greatest(u.created_at, coalesce(u.released_at, u.created_at)) < now() - make_interval(days => %s) "
        "AND NOT EXISTS (SELECT 1 FROM beta_keys k WHERE k.user_id=u.id AND k.bound_at IS NOT NULL) "
        "AND NOT EXISTS (SELECT 1 FROM reports r WHERE r.user_id=u.id)", INACTIVE_DAYS)]


def prune_inactive():
    """Idempotente (roda no laço de sync de qualquer worker). Devolve os ids que saíram."""
    return disable(inactive_ids(), "inactive")
