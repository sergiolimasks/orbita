import base64, io, re, secrets, time, unicodedata

import psycopg
import segno
from flask import Blueprint, abort, current_app, g, redirect, render_template, request, session, url_for

from .. import bridgekeys, countries, db, security as sec
from ..i18n import gettext as _

bp = Blueprint("auth", __name__)

EMAIL_RE = re.compile(r"^[^@\s]{1,64}@[^@\s]{1,253}\.[^@\s.]{2,}$")
PHONE_RE = re.compile(r"^\+?[0-9][0-9 ().-]{5,22}$")
MFA_TTL = 300


def make_handle(name):
    s = unicodedata.normalize("NFKD", name)
    s = re.sub(r"[^a-z0-9]+", "-", s.encode("ascii", "ignore").decode().lower()).strip("-")[:20] or "t"
    while True:
        h = "%s-%s" % (s, secrets.token_hex(2))
        if not db.row("SELECT 1 FROM users WHERE handle=%s", h):
            return h


# ------------------------------------------------------------------ login
@bp.route("/login", methods=["GET", "POST"])
def login():
    if g.user:
        return redirect(url_for("admin.overview" if g.user["role"] == "admin" else "tester.area"))
    nxt = sec.safe_next(request.values.get("next"))
    if request.method == "GET":
        return render_template("login.html", next=nxt)
    if sec.ip_blocked("login"):
        abort(429)
    email = (request.form.get("email") or "").strip().lower()[:320]
    pw = request.form.get("password") or ""
    u = db.row("SELECT * FROM users WHERE email=%s", email)
    if u and sec.is_locked(u):
        sec.check_pw(None, pw)
        sec.record_attempt("login", False)
        return render_template("login.html", next=nxt, error=_("login.locked"), email=email), 429
    if not u or not u["active"] or not sec.check_pw(u["pw_hash"], pw):
        if u:
            sec.register_fail(u)
        sec.record_attempt("login", False)
        return render_template("login.html", next=nxt, error=_("login.error"), email=email), 401
    sec.record_attempt("login", True)
    if u["role"] == "admin" or u["totp_enabled"]:
        session.clear()
        session["mfa_uid"], session["mfa_at"], session["mfa_next"] = u["id"], int(time.time()), nxt
        session["csrf"] = sec.token()
        return redirect(url_for("auth.login_2fa"))
    sec.start_session(u)
    return redirect(nxt or url_for("tester.area"))


@bp.route("/login/2fa", methods=["GET", "POST"])
def login_2fa():
    uid, at = session.get("mfa_uid"), session.get("mfa_at", 0)
    if not uid or time.time() - at > MFA_TTL:
        session.clear()
        return render_template("login.html", error=_("login.totp_expired")), 401
    if request.method == "GET":
        return render_template("login_2fa.html")
    if sec.ip_blocked("totp"):
        abort(429)
    u = db.row("SELECT * FROM users WHERE id=%s AND active", uid)
    if not u or sec.is_locked(u):
        session.clear()
        return render_template("login.html", error=_("login.locked")), 429
    step = sec.totp_check(sec.dec(u["totp_enc"] or ""), request.form.get("code"), u["totp_last_step"])
    if step is None:
        sec.register_fail(u)
        sec.record_attempt("totp", False)
        return render_template("login_2fa.html", error=_("login.totp_error")), 401
    # grava o passo (impede reuso do mesmo código) de forma atômica
    if not db.run("UPDATE users SET totp_last_step=%s WHERE id=%s AND totp_last_step < %s", step, u["id"], step):
        return render_template("login_2fa.html", error=_("login.totp_error")), 401
    sec.record_attempt("totp", True)
    nxt = session.get("mfa_next")
    sec.start_session(u)
    return redirect(nxt or url_for("admin.overview" if u["role"] == "admin" else "tester.area"))


@bp.post("/logout")
def logout():
    sec.end_session()
    return redirect(url_for("public.home"))


# ------------------------------------------------------------------ convite -> cadastro
def _invite(tok):
    return db.row("SELECT * FROM invites WHERE token_sha256=%s AND used_at IS NULL AND revoked_at IS NULL "
                  "AND expires_at > now()", sec.sha256(tok or ""))


@bp.route("/invite/<tok>", methods=["GET", "POST"])
def signup(tok):
    if len(tok) > 100:
        abort(404)
    inv = _invite(tok)
    if not inv:
        return render_template("message.html", title=_("signup.invalid_title"), text=_("signup.invalid")), 410
    clist = countries.countries(g.lang)
    if request.method == "GET":
        return render_template("signup.html", inv=inv, countries=clist, f={})
    if sec.ip_blocked("signup"):
        abort(429)
    f = {k: (request.form.get(k) or "").strip() for k in ("name", "email", "phone", "country")}
    f["email"] = f["email"].lower()
    f["accept"] = request.form.get("accept") == "1"   # não desmarca o aceite quando o formulário volta com erro
    pw, pw2 = request.form.get("password") or "", request.form.get("password2") or ""
    errs = []
    if not 1 <= len(f["name"]) <= 80:
        errs.append(("name", _("err.name")))
    if not EMAIL_RE.match(f["email"]) or len(f["email"]) > 254:
        errs.append(("email", _("err.email")))
    if not PHONE_RE.match(f["phone"]):
        errs.append(("phone", _("err.phone")))
    if f["country"] not in countries.codes():
        errs.append(("country", _("err.country")))
    if not sec.pw_ok(pw):
        errs.append(("password", _("err.pw")))
    elif pw != pw2:
        errs.append(("password2", _("err.pw2")))
    if not f["accept"]:
        errs.append(("accept", _("err.accept")))
    if not errs and db.row("SELECT 1 FROM users WHERE email=%s", f["email"]):
        errs.append(("email", _("err.email_taken")))
    if errs:
        sec.record_attempt("signup", False)
        return render_template("signup.html", inv=inv, countries=clist, f=f, errors=[e for _k, e in errs], ferr=dict(errs)), 400
    handle, pw_hash = make_handle(f["name"]), sec.hash_pw(pw)
    try:
        with db.conn() as c:
            used = c.execute("UPDATE invites SET used_at=now() WHERE id=%s AND used_at IS NULL AND revoked_at IS NULL "
                             "AND expires_at > now() RETURNING id", (inv["id"],)).fetchone()
            if not used:
                raise LookupError
            u = c.execute("INSERT INTO users(role, name, email, phone, country, handle, pw_hash) "
                          "VALUES ('tester',%s,%s,%s,%s,%s,%s) RETURNING *",
                          (f["name"], f["email"], f["phone"], f["country"], handle, pw_hash)).fetchone()
            c.execute("UPDATE invites SET used_by=%s WHERE id=%s", (u["id"], inv["id"]))
            new_key = bridgekeys.new_key(u, conn=c)
    except LookupError:
        return render_template("message.html", title=_("signup.invalid_title"), text=_("signup.invalid")), 410
    except psycopg.errors.UniqueViolation:
        return render_template("signup.html", inv=inv, countries=clist, f=f, errors=[_("err.email_taken")],
                               ferr={"email": _("err.email_taken")}), 400
    sec.start_session(u)
    g.user = u
    from .tester import _area_ctx
    return render_template("area.html", new_key=new_key, **_area_ctx())


# ------------------------------------------------------------------ configuração do admin (link de uso único)
def _setup_link(tok):
    return db.row("SELECT * FROM setup_links WHERE token_sha256=%s AND used_at IS NULL AND expires_at > now()",
                  sec.sha256(tok or ""))


def _qr_data_uri(uri):
    # svg_inline() sai sem xmlns: dentro de <img> (data URI) o navegador não renderiza
    buf = io.BytesIO()
    segno.make(uri, error="m").save(buf, kind="svg", scale=5, dark="#1B1C1E", light="#FFFFFF")
    return "data:image/svg+xml;base64," + base64.b64encode(buf.getvalue()).decode()


@bp.route("/setup/<tok>", methods=["GET", "POST"])
def setup(tok):
    if len(tok) > 100:
        abort(404)
    link = _setup_link(tok)
    if not link:
        return render_template("message.html", title=_("setup.title"), text=_("setup.invalid")), 410
    if request.method == "GET":
        return render_template("setup.html", f={})
    if sec.ip_blocked("setup"):
        abort(429)
    f = {k: (request.form.get(k) or "").strip() for k in ("name", "email")}
    f["email"] = f["email"].lower()
    pw, pw2 = request.form.get("password") or "", request.form.get("password2") or ""
    errs = []
    if not 1 <= len(f["name"]) <= 80:
        errs.append(("name", _("err.name")))
    if not EMAIL_RE.match(f["email"]):
        errs.append(("email", _("err.email")))
    if not sec.pw_ok(pw):
        errs.append(("password", _("err.pw")))
    elif pw != pw2:
        errs.append(("password2", _("err.pw2")))
    other = db.row("SELECT id FROM users WHERE email=%s", f["email"])
    if other and other["id"] != link["user_id"]:
        errs.append(("email", _("err.email_taken")))
    if errs:
        sec.record_attempt("setup", False)
        return render_template("setup.html", f=f, errors=[e for _k, e in errs], ferr=dict(errs)), 400
    secret = sec.new_totp_secret()
    with db.conn() as c:
        if link["user_id"]:
            c.execute("UPDATE users SET name=%s, email=%s, pw_hash=%s, totp_enc=%s WHERE id=%s AND NOT active",
                      (f["name"], f["email"], sec.hash_pw(pw), sec.enc(secret), link["user_id"]))
        else:
            u = c.execute("INSERT INTO users(role, name, email, handle, pw_hash, totp_enc, active) "
                          "VALUES ('admin',%s,%s,%s,%s,%s,false) RETURNING id",
                          (f["name"], f["email"], make_handle("admin"), sec.hash_pw(pw), sec.enc(secret))).fetchone()
            c.execute("UPDATE setup_links SET user_id=%s WHERE token_sha256=%s", (u["id"], link["token_sha256"]))
    return redirect(url_for("auth.setup_2fa", tok=tok))


@bp.route("/setup/<tok>/2fa", methods=["GET", "POST"])
def setup_2fa(tok):
    link = _setup_link(tok)
    if not link or not link["user_id"]:
        return render_template("message.html", title=_("setup.title"), text=_("setup.invalid")), 410
    u = db.row("SELECT * FROM users WHERE id=%s AND NOT active", link["user_id"])
    if not u:
        abort(410)
    secret = sec.dec(u["totp_enc"])
    ctx = {"secret": secret, "qr": _qr_data_uri(sec.totp_uri(secret, u["email"], current_app.config["BRAND"]["name"]))}
    if request.method == "GET":
        return render_template("setup_2fa.html", **ctx)
    if sec.ip_blocked("setup"):
        abort(429)
    step = sec.totp_check(secret, request.form.get("code"), 0)
    if step is None:
        sec.record_attempt("setup", False)
        return render_template("setup_2fa.html", error=_("login.totp_error"), **ctx), 400
    with db.conn() as c:
        ok = c.execute("UPDATE setup_links SET used_at=now() WHERE token_sha256=%s AND used_at IS NULL RETURNING 1",
                       (link["token_sha256"],)).fetchone()
        if not ok:
            abort(410)
        c.execute("UPDATE users SET active=true, totp_enabled=true, totp_last_step=%s WHERE id=%s", (step, u["id"]))
    u = db.row("SELECT * FROM users WHERE id=%s", u["id"])
    sec.start_session(u)
    return redirect(url_for("admin.overview"))
