"""Senhas (argon2id), tokens, sessões no banco, CSRF, rate limit, TOTP, chaves beta."""
import hashlib, hmac, re, secrets, time
from datetime import timedelta
from functools import wraps

import pyotp
from argon2 import PasswordHasher
from argon2 import exceptions as aex
from cryptography.fernet import Fernet, InvalidToken
from flask import abort, current_app, g, redirect, request, session, url_for

from . import db

# argon2id; 19 MiB / t=3 / p=1 (mínimo OWASP) pra caber no limite de memória do container
PH = PasswordHasher(time_cost=3, memory_cost=19456, parallelism=1)
_DUMMY = PH.hash("dummy-password-for-constant-time")

PW_MIN, PW_MAX = 10, 128
KEY_ALPHA = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"   # o mesmo de tools/beta_keys.py (sem 0/O/1/I/L)


def sha256(s):
    return hashlib.sha256(s.encode()).hexdigest()


def token():
    return secrets.token_urlsafe(32)


# ------------------------------------------------------------------ senhas
def hash_pw(p):
    return PH.hash(p)


def check_pw(h, p):
    try:
        ok = PH.verify(h or _DUMMY, p or "")
        return bool(ok and h)
    except (aex.VerificationError, aex.InvalidHashError, aex.VerifyMismatchError):
        return False


def pw_ok(p):
    return isinstance(p, str) and PW_MIN <= len(p) <= PW_MAX


# ------------------------------------------------------------------ cifra
def _fernet():
    return Fernet(current_app.config["KEYENC"].encode())


def enc(s):
    return _fernet().encrypt(s.encode()).decode()


def dec(s):
    try:
        return _fernet().decrypt(s.encode()).decode()
    except (InvalidToken, AttributeError):
        return None


# ------------------------------------------------------------------ TOTP
def new_totp_secret():
    return pyotp.random_base32()


def totp_uri(secret, email, issuer):
    return pyotp.TOTP(secret).provisioning_uri(name=email, issuer_name=issuer)


def totp_check(secret, code, last_step):
    """Devolve o passo aceito (pra gravar e impedir reuso) ou None."""
    code = re.sub(r"\s", "", code or "")
    if not secret or not re.fullmatch(r"\d{6}", code):
        return None
    t = pyotp.TOTP(secret)
    now = int(time.time()) // 30
    for st in (now - 1, now, now + 1):
        if st > (last_step or 0) and hmac.compare_digest(t.at(st * 30), code):
            return st
    return None


# ------------------------------------------------------------------ chaves beta
def norm_key(k):
    return re.sub(r"[\s-]", "", k or "").upper()


def key_hash(k):
    """Contrato com a ponte: sha256 hex de UPPER(chave sem '-' e espaços)."""
    return sha256(norm_key(k))


# ------------------------------------------------------------------ IP / rate limit
def client_ip():
    return request.remote_addr or "?"


def ip_blocked(kind):
    r = db.row("SELECT count(*) AS n FROM login_attempts WHERE ip=%s AND kind=%s AND NOT ok "
               "AND at > now() - interval '15 minutes'", client_ip(), kind)
    return r["n"] >= current_app.config["IP_MAX_FAILS"]


def record_attempt(kind, ok):
    db.run("INSERT INTO login_attempts(ip, kind, ok) VALUES (%s,%s,%s)", client_ip(), kind, ok)


def register_fail(user):
    """Conta falha na conta; bloqueia após LOGIN_MAX_FAILS."""
    cfg = current_app.config
    db.run("UPDATE users SET "
           "locked_until = CASE WHEN failed_logins + 1 >= %s THEN now() + make_interval(mins => %s) "
           "ELSE locked_until END, "
           "failed_logins = CASE WHEN failed_logins + 1 >= %s THEN 0 ELSE failed_logins + 1 END "
           "WHERE id=%s", cfg["LOGIN_MAX_FAILS"], cfg["LOCK_MINUTES"], cfg["LOGIN_MAX_FAILS"], user["id"])


def is_locked(user):
    r = db.row("SELECT locked_until > now() AS l FROM users WHERE id=%s", user["id"])
    return bool(r and r["l"])


# ------------------------------------------------------------------ sessão
def start_session(user):
    session.clear()
    raw = token()
    ttl = timedelta(hours=12) if user["role"] == "admin" else timedelta(days=7)
    db.run("INSERT INTO sessions(id_sha256, user_id, expires_at, ip, ua) VALUES (%s,%s,now()+%s,%s,%s)",
           sha256(raw), user["id"], ttl, client_ip(), (request.user_agent.string or "")[:200])
    db.run("UPDATE users SET last_login_at=now(), failed_logins=0, locked_until=NULL WHERE id=%s", user["id"])
    session.permanent = True
    session["sid"] = raw
    session["csrf"] = token()


def load_user():
    g.user = None
    raw = session.get("sid")
    if not raw:
        return
    u = db.row("SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id "
               "WHERE s.id_sha256=%s AND s.expires_at > now() AND u.active "
               "AND (u.role <> 'admin' OR s.last_seen > now() - interval '2 hours')", sha256(raw))
    if not u:
        session.pop("sid", None)
        return
    db.run("UPDATE sessions SET last_seen=now() WHERE id_sha256=%s", sha256(raw))
    g.user = u


def end_session():
    raw = session.get("sid")
    if raw:
        db.run("DELETE FROM sessions WHERE id_sha256=%s", sha256(raw))
    session.clear()


def kill_sessions(user_id):
    db.run("DELETE FROM sessions WHERE user_id=%s", user_id)


# ------------------------------------------------------------------ CSRF
def csrf_token():
    if "csrf" not in session:
        session["csrf"] = token()
    return session["csrf"]


def check_csrf():
    if request.method in ("POST", "PUT", "PATCH", "DELETE"):
        if request.path.startswith("/_bridge/"):     # ponte: X-Bridge-Token (views/bridge.py)
            return
        sent = request.form.get("csrf") or request.headers.get("X-CSRF-Token", "")
        if not sent or not hmac.compare_digest(sent, session.get("csrf", "")):
            abort(400)


# ------------------------------------------------------------------ decorators
def safe_next(n):
    return n if isinstance(n, str) and n.startswith("/") and not n.startswith("//") and "\\" not in n else None


def login_required(f):
    @wraps(f)
    def w(*a, **k):
        if not g.user:
            return redirect(url_for("auth.login", next=request.path))
        return f(*a, **k)
    return w


def admin_required(f):
    @wraps(f)
    def w(*a, **k):
        if not g.user:
            return redirect(url_for("auth.login", next=request.path))
        if g.user["role"] != "admin" or not g.user["totp_enabled"]:
            abort(403)
        return f(*a, **k)
    return w


def audit(action, detail=""):
    db.run("INSERT INTO audit(user_id, action, detail) VALUES (%s,%s,%s)",
           g.user["id"] if getattr(g, "user", None) else None, action, str(detail)[:500])
