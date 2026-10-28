"""Site da beta (x360.pistatrack.com): página pública, login, testadores, painel admin."""
import json, os
from datetime import timedelta

from flask import Flask, g, render_template, request
from werkzeug.middleware.proxy_fix import ProxyFix

from . import config, countries, db, i18n, icons, logindex, security, seed, ui
from .storage import R2Storage

HERE = os.path.dirname(__file__)
ASSET_V = "8"   # sobe quando app.css/app.js mudam (cache do navegador)


def _query_with(**kw):
    """Query string atual com chaves trocadas (ex.: trocar idioma sem perder filtros)."""
    from urllib.parse import urlencode
    args = {k: v for k, v in request.args.items() if k not in kw}
    args.update({k: v for k, v in kw.items() if v is not None})
    return urlencode(args)


def _nav_counts():
    """Números da barra lateral (uma consulta por página, só pra quem está logado)."""
    if "nav_counts" in g:
        return g.nav_counts
    from . import releases
    u = g.get("user")
    out = {}
    if u and u["role"] == "admin":
        out = db.row("""SELECT
            (SELECT count(*) FROM reports WHERE status='new') AS new_reports,
            (SELECT count(*) FROM invites WHERE used_at IS NULL AND revoked_at IS NULL AND expires_at > now()) AS invites,
            (SELECT count(*) FROM promotion_requests WHERE done_at IS NULL) AS promotions,
            (SELECT count(*) FROM users WHERE wait='waiting' AND active) AS waitlist,
            (SELECT count(*) FROM beta_keys k JOIN users u ON u.id=k.user_id
              WHERE k.revoked_at IS NULL AND k.dup_at IS NOT NULL AND u.active) AS blocked_keys""")
    elif u:
        out = db.row("SELECT count(*) FILTER (WHERE status = ANY(%s)) AS my_open FROM reports WHERE user_id=%s",
                     list(i18n.OPEN_STATUSES), u["id"])
    out = dict(out or {})
    b = releases.manifest("beta")
    out["beta"] = b["version"] if b else None
    g.nav_counts = out
    return out


def load_brand():
    """Nome/logo/cores num lugar só: xc360site/brand.json (+ static/brand/)."""
    with open(os.path.join(HERE, "brand.json"), encoding="utf-8") as f:
        return json.load(f)


def create_app(overrides=None, storage=None):
    app = Flask(__name__)
    app.config.update(config.load())
    app.config.update(overrides or {})
    for k in ("SECRET_KEY", "KEYENC", "DATABASE_URL"):
        if not app.config.get(k):
            raise RuntimeError("configuração ausente: " + k)
    app.config.update(
        BRAND=load_brand(),
        SESSION_COOKIE_NAME="__Host-xc360",
        SESSION_COOKIE_SECURE=True,
        SESSION_COOKIE_HTTPONLY=True,
        SESSION_COOKIE_SAMESITE="Lax",
        PERMANENT_SESSION_LIFETIME=timedelta(days=7),
        TEMPLATES_AUTO_RELOAD=False,
    )
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1)

    db.init(app.config["DATABASE_URL"])
    db.apply_schema()
    seed.run()
    app.extensions["storage"] = storage or R2Storage.from_env_text(app.config["R2_ENV"])

    @app.before_request
    def _before():
        g.lang, g.set_lang = i18n.pick_lang()
        security.load_user()
        security.check_csrf()

    @app.after_request
    def _after(resp):
        if g.get("set_lang"):
            resp.set_cookie("lang", g.lang, max_age=365 * 86400, secure=True, samesite="Lax")
        frame = "https://www.youtube-nocookie.com https://www.youtube.com https://player.vimeo.com"
        resp.headers.setdefault("Content-Security-Policy",
                                "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; "
                                "font-src 'self'; frame-src %s; frame-ancestors 'none'; form-action 'self'; "
                                "base-uri 'none'; object-src 'none'" % frame)
        resp.headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains"
        resp.headers["X-Content-Type-Options"] = "nosniff"
        resp.headers["X-Frame-Options"] = "DENY"
        resp.headers["Referrer-Policy"] = "same-origin"
        resp.headers["Permissions-Policy"] = "camera=(), microphone=(), geolocation=(), payment=()"
        resp.headers["Cross-Origin-Opener-Policy"] = "same-origin"
        if g.get("user") or request.path.startswith(("/admin", "/area", "/login", "/invite", "/setup")):
            resp.headers["Cache-Control"] = "no-store"
        resp.vary.add("Cookie")
        return resp

    @app.context_processor
    def _ctx():
        return {
            "_": i18n.gettext,
            "lang": g.get("lang", i18n.DEFAULT),
            "LANGS": i18n.LANGS,
            "LANG_LABEL": i18n.LANG_LABEL,
            "brand": app.config["BRAND"],
            "user": g.get("user"),
            "csrf_token": security.csrf_token,
            "country_name": lambda c: countries.name(c, g.get("lang", i18n.DEFAULT)),
            "flag": ui.flag,
            "initials": ui.initials,
            "icon": icons.icon,
            "loc": lambda row, field: ui.loc(row, field, g.get("lang", i18n.DEFAULT)),
            "LANG_NAME": i18n.LANG_NAME,
            "GROUP_ICON": {"controls": "gamepad", "video": "monitor", "menu": "layers", "system": "settings"},
            "asset_v": ASSET_V,
            "query_with": _query_with,
            "nav_counts": _nav_counts,
            "beta_now": _beta_now,
        }

    def _beta_now():
        """Versão beta atual (manifesto, cache de 60 s) e se ela tem página de novidades publicada."""
        from . import releases
        v = (releases.manifest("beta") or {}).get("version")
        if not v:
            return None
        try:
            has_note = bool(db.row("SELECT 1 AS x FROM release_notes WHERE version=%s AND published", v))
        except Exception:
            has_note = False
        return {"version": v, "has_note": has_note}

    def _lang():
        return g.get("lang", i18n.DEFAULT)

    @app.template_filter("dt")
    def _dt(v):
        """Data/hora em texto (UTC), no formato do idioma. Seguro dentro de atributos/frases."""
        return ui.fmt_dt(v, _lang()) + " UTC" if ui.to_dt(v) else "—"

    @app.template_filter("date")
    def _date(v, short=False):
        return ui.fmt_date(v, _lang(), short)

    @app.template_filter("when")
    def _when(v):
        """<time> que o app.js converte pro fuso local do navegador."""
        return ui.when(v, _lang())

    @app.template_filter("ago")
    def _ago(v):
        return ui.ago(v, _lang())

    @app.template_filter("size")
    def _size(n):
        n = n or 0
        return "%.1f MB" % (n / 1048576) if n >= 1048576 else "%d KB" % max(1, n // 1024)

    def _err(code):
        def h(e):
            return render_template("error.html", code=code), code
        return h
    for code in (400, 403, 404, 413, 429, 500):
        app.register_error_handler(code, _err(code))

    from .views import admin, auth, bridge, public, tester
    for bp in (public.bp, auth.bp, tester.bp, admin.bp, bridge.bp):
        app.register_blueprint(bp)

    if not app.config.get("TESTING"):
        logindex.start_thread(app)
    return app
