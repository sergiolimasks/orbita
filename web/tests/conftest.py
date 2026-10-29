"""Testes contra um Postgres de verdade (TEST_DATABASE_URL) e R2 em memória."""
import os, re, time

import pyotp
import pytest
from cryptography.fernet import Fernet
from flask.testing import FlaskClient

from xc360site import create_app, db, security
from xc360site.storage import MemStorage

TABLES = "known_issues, game_flags, play_sessions, key_events, release_note_items, release_notes, features, report_events, audit, promotion_requests, kv, log_index, report_files, reports, beta_keys, login_attempts, sessions, " \
         "setup_links, invites, users"


class HttpsClient(FlaskClient):
    """Tudo via https (o cookie __Host- exige Secure)."""
    def open(self, *a, **kw):
        if not (a and not isinstance(a[0], str)):
            kw.setdefault("base_url", "https://x360.test")
        return super().open(*a, **kw)


@pytest.fixture(scope="session")
def _app(tmp_path_factory):
    dsn = os.environ["TEST_DATABASE_URL"]
    app = create_app({
        "TESTING": True, "SECRET_KEY": "test-secret", "KEYENC": Fernet.generate_key().decode(),
        "DATABASE_URL": dsn, "BASE_URL": "https://x360.test", "LOG_SYNC_INTERVAL": 0,
        "BRIDGE_TOKEN": BRIDGE_TOKEN,
    }, storage=MemStorage())
    app.test_client_class = HttpsClient
    return app


@pytest.fixture
def app(_app):
    db.run("TRUNCATE " + TABLES + " RESTART IDENTITY CASCADE")
    _app.extensions["storage"].objs.clear()
    from xc360site import releases
    releases.clear_cache()
    return _app


@pytest.fixture
def client(app):
    return app.test_client()


BRIDGE_TOKEN = "bridge-test-token"
CON_A, CON_B, CON_C = "a" * 32, "b" * 32, "c" * 32


def bcheck(client, key, console=CON_A, token=BRIDGE_TOKEN, ip_h="1234abcd", headers=None, bind=None):
    """O que a ponte faz: POST /_bridge/check com sha256 da chave normalizada."""
    h = {"X-Bridge-Token": token} if token is not None else {}
    h.update(headers or {})
    body = {"key_sha256": security.key_hash(key), "console": console, "ip_h": ip_h}
    if bind:
        body["bind"] = bind
    r = client.post("/_bridge/check", json=body, headers=h)
    return r.get_json()["status"] if r.status_code == 200 else r.status_code


CSRF_RE = re.compile(r'name="csrf" value="([^"]+)"')


def csrf(client, path="/login"):
    html = client.get(path).get_data(as_text=True)
    m = CSRF_RE.search(html)
    assert m, "sem token CSRF em " + path
    return m.group(1)


def post(client, path, data, page=None, **kw):
    data = dict(data)
    data["csrf"] = csrf(client, page or path)
    return client.post(path, data=data, **kw)


def make_user(app, role="tester", email="t@example.com", pw="senha-forte-123", totp=False, active=True, name="Tester"):
    with app.app_context():
        secret = pyotp.random_base32() if totp else None
        u = db.row("INSERT INTO users(role, name, email, handle, pw_hash, totp_enc, totp_enabled, active) "
                   "VALUES (%s,%s,%s,%s,%s,%s,%s,%s) RETURNING *",
                   role, name, email, "h-" + email.split("@")[0], security.hash_pw(pw),
                   security.enc(secret) if secret else None, totp, active)
        u["_totp"] = secret
        return u


def login(client, email="t@example.com", pw="senha-forte-123"):
    return post(client, "/login", {"email": email, "password": pw})


def login_admin(client, app, email="admin@example.com", pw="senha-forte-123"):
    u = make_user(app, role="admin", email=email, pw=pw, totp=True, name="Admin")
    r = login(client, email, pw)
    assert r.status_code == 302 and r.headers["Location"].endswith("/login/2fa")
    code = pyotp.TOTP(u["_totp"]).now()
    r = post(client, "/login/2fa", {"code": code})
    assert r.status_code == 302, r.get_data(as_text=True)[:500]
    return u


def new_invite(client, amount="2", unit="days"):
    r = post(client, "/admin/invites", {"amount": amount, "unit": unit, "note": "fulano"})
    m = re.search(r'id="invlink">([^<]+)<', r.get_data(as_text=True))
    return r, (m.group(1) if m else None)
