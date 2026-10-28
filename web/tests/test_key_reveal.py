"""v0.21.38: o dono vê a chave atual de novo na área (e só a vigente)."""
from conftest import login, make_user, post
from xc360site import bridgekeys, db


def test_owner_sees_current_key_again(app, client):
    u = make_user(app, email="revela@example.com")
    with app.app_context():
        k1 = bridgekeys.new_key(db.row("SELECT * FROM users WHERE id=%s", u["id"]))
        k2 = bridgekeys.new_key(db.row("SELECT * FROM users WHERE id=%s", u["id"]))
        assert bridgekeys.reveal(u["id"]) == k2
    login(client, email="revela@example.com")
    html = client.get("/area").get_data(as_text=True)
    assert k2 in html and k1 not in html


def test_revoked_key_not_revealed(app):
    u = make_user(app, email="sem@example.com")
    with app.app_context():
        bridgekeys.new_key(db.row("SELECT * FROM users WHERE id=%s", u["id"]))
        bridgekeys.revoke_user_keys(u["id"])
        assert bridgekeys.reveal(u["id"]) is None
