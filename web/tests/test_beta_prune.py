"""Beta só pra quem testa: desativação com motivo (mensagem no login) e limpeza dos inativos de 7 dias."""
from conftest import make_user, post
from xc360site import beta_prune, bridgekeys, db


def test_disabled_reason_shows_message_only_with_right_password(app, client):
    uid = make_user(app, email="sumido@example.com")["id"]
    with app.app_context():
        bridgekeys.new_key(db.row("SELECT * FROM users WHERE id=%s", uid))
        assert beta_prune.disable([uid], "noreport") == [uid]
        assert db.row("SELECT count(*) AS n FROM beta_keys WHERE user_id=%s AND revoked_at IS NULL", uid)["n"] == 0
    r = post(client, "/login", {"email": "sumido@example.com", "password": "senha-forte-123"})
    assert r.status_code == 403 and "official release" in r.get_data(as_text=True)
    r = post(client, "/login", {"email": "sumido@example.com", "password": "errada-errada-1"})
    assert r.status_code == 401 and "official release" not in r.get_data(as_text=True)


def test_prune_inactive_after_7_days(app):
    old = make_user(app, email="velho@example.com")["id"]
    new = make_user(app, email="novo@example.com")["id"]
    used = make_user(app, email="usou@example.com")["id"]
    with app.app_context():
        db.run("UPDATE users SET created_at = now() - interval '8 days' WHERE id = ANY(%s)", [old, used])
        u = db.row("SELECT * FROM users WHERE id=%s", used)
        bridgekeys.new_key(u)
        db.run("UPDATE beta_keys SET bound_at=now(), console_id='abc' WHERE user_id=%s", used)
        assert beta_prune.prune_inactive() == [old]
        assert db.row("SELECT active, disabled_reason FROM users WHERE id=%s", old) == {"active": False, "disabled_reason": "inactive"}
        assert db.row("SELECT active FROM users WHERE id=%s", new)["active"]
        assert db.row("SELECT active FROM users WHERE id=%s", used)["active"]
