import re
from datetime import date, timedelta

from flask import (Blueprint, Response, abort, current_app, flash, g, redirect, render_template, request,
                   url_for)

from .. import bridgekeys, db, logindex, plays, releases, security as sec, ui, waitlist
from ..i18n import OPEN_STATUSES, STATUSES, gettext as _

bp = Blueprint("admin", __name__, url_prefix="/admin")
DAY_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
ERR_RE = re.compile(r"(error|erro|fail|falh|fatal|panic|abort|assert|exception|crash|red ?screen|tela vermelha|timeout|\bE/)", re.I)
WARN_RE = re.compile(r"(warn|aviso|retry|drop|lost|perdid|slow|lento|\bW/)", re.I)
PER_PAGE = 20


@bp.before_request
@sec.admin_required
def _guard():
    return None


def _storage():
    return current_app.extensions["storage"]


def _like(s):
    return "%" + s.replace("\\", "\\\\").replace("%", r"\%").replace("_", r"\_") + "%"


def _pending_promotions():
    st = releases.manifest("stable")
    if st:
        db.run("UPDATE promotion_requests SET done_at=now() WHERE done_at IS NULL AND version=%s", st["version"])
    return db.rows("SELECT * FROM promotion_requests WHERE done_at IS NULL ORDER BY id DESC")


def _synced():
    r = db.row("SELECT v FROM kv WHERE k='logs_synced_at'")
    return r["v"] if r else None


# ------------------------------------------------------------------ testador nos filtros
DEV_TESTER = "dev"   # campo "tester" dos lotes da chave de desenvolvimento (validada só na ponte)


def _dev_owner():
    """Admin dono da chave dev (o primeiro admin)."""
    return db.row("SELECT * FROM users WHERE role='admin' ORDER BY id LIMIT 1")


def _find_user(v):
    """?tester= aceita id, handle ou "dev" (-> admin dono). None se não existir."""
    v = (v or "").strip()[:64]
    if not v:
        return None
    if v == DEV_TESTER:
        return _dev_owner()
    if v.isdigit():
        return db.row("SELECT * FROM users WHERE id=%s", int(v)) if len(v) < 10 else None
    return db.row("SELECT * FROM users WHERE handle=%s", v)


def _log_handles(u):
    """Valores de log_index.tester que são deste usuário (o admin dono também leva os lotes "dev")."""
    hs = [u["handle"]]
    if u["role"] == "admin":
        o = _dev_owner()
        if o and o["id"] == u["id"]:
            hs.append(DEV_TESTER)
    return hs


def _log_names():
    names = {r["handle"]: r["name"] for r in db.rows("SELECT handle, name FROM users")}
    names[DEV_TESTER] = _("adm.dev_tester")
    return names


# ------------------------------------------------------------------ visão geral
@bp.get("/")
def overview():
    n = db.row("""SELECT
        (SELECT count(*) FROM users WHERE role='tester' AND active) AS testers,
        (SELECT count(*) FROM users WHERE role='tester' AND created_at > now() - interval '7 days') AS testers_7,
        (SELECT count(*) FROM users WHERE role='tester' AND created_at <= now() - interval '7 days'
                                      AND created_at > now() - interval '14 days') AS testers_p7,
        (SELECT count(*) FROM invites WHERE used_at IS NULL AND revoked_at IS NULL AND expires_at > now()) AS invites,
        (SELECT count(*) FROM invites WHERE used_at > now() - interval '30 days') AS invites_used_30,
        (SELECT count(*) FROM reports WHERE status = ANY(%s)) AS open_reports,
        (SELECT count(*) FROM reports WHERE status='new') AS new_reports,
        (SELECT count(*) FROM reports) AS reports,
        (SELECT count(*) FROM reports WHERE created_at > now() - interval '7 days') AS reports_7,
        (SELECT count(*) FROM reports WHERE created_at <= now() - interval '7 days'
                                        AND created_at > now() - interval '14 days') AS reports_p7,
        (SELECT count(DISTINCT console) FROM log_index WHERE day > current_date - 7) AS consoles,
        (SELECT count(DISTINCT console) FROM log_index WHERE day <= current_date - 7 AND day > current_date - 14) AS consoles_p7,
        (SELECT count(*) FROM log_index WHERE day > current_date - 7) AS batches_7,
        (SELECT count(*) FROM log_index WHERE day <= current_date - 7 AND day > current_date - 14) AS batches_p7""",
               list(OPEN_STATUSES))
    days = db.rows("""SELECT d::date AS day,
        (SELECT count(*) FROM log_index WHERE day = d::date) AS batches,
        (SELECT count(DISTINCT console) FROM log_index WHERE day = d::date) AS consoles,
        (SELECT count(*) FROM reports WHERE (created_at AT TIME ZONE 'UTC')::date = d::date) AS reports,
        (SELECT count(*) FROM reports WHERE (created_at AT TIME ZONE 'UTC')::date <= d::date AND status = ANY(%s)) AS open_cum,
        (SELECT count(*) FROM users WHERE role='tester' AND (created_at AT TIME ZONE 'UTC')::date <= d::date) AS testers
        FROM generate_series(current_date - 29, current_date, interval '1 day') d ORDER BY 1""", list(OPEN_STATUSES))
    last14 = days[-14:]
    lang = g.lang
    charts = {
        "batches": ui.bar_chart([(ui.fmt_date(d["day"], lang, True), d["batches"]) for d in days]),
        "reports": ui.bar_chart([(ui.fmt_date(d["day"], lang, True), d["reports"]) for d in days], h=120),
    }
    sparks = {
        "testers": ui.spark([d["testers"] for d in last14]),
        "open": ui.spark([d["open_cum"] for d in last14]),
        "consoles": ui.spark([d["consoles"] for d in last14]),
        "batches": ui.spark([d["batches"] for d in last14]),
    }
    deltas = {
        "testers": ui.delta(n["testers_7"], n["testers_p7"]),
        "reports": ui.delta(n["reports_7"], n["reports_p7"]),
        "consoles": ui.delta(n["consoles"], n["consoles_p7"]),
        "batches": ui.delta(n["batches_7"], n["batches_p7"]),
    }
    totals = {"batches": sum(d["batches"] for d in days), "reports": sum(d["reports"] for d in days)}
    by_status = db.rows("SELECT status, count(*) AS n FROM reports GROUP BY status ORDER BY n DESC")
    vers = db.rows("SELECT app, count(DISTINCT console) AS consoles, count(*) AS batches FROM log_index "
                   "WHERE day > current_date - 7 AND app NOT IN ('', '?') GROUP BY app ORDER BY consoles DESC, app DESC LIMIT 8")
    games = db.rows("SELECT game, count(*) AS batches, count(DISTINCT console) AS consoles FROM log_index "
                    "WHERE day > current_date - 30 AND game NOT IN ('', '?') GROUP BY game ORDER BY batches DESC LIMIT 8")
    recent = db.rows("SELECT r.id, r.status, r.description, r.game, r.version, r.created_at, u.name AS uname, "
                     "(SELECT count(*) FROM report_files f WHERE f.report_id=r.id) AS nfiles "
                     "FROM reports r JOIN users u ON u.id=r.user_id ORDER BY r.id DESC LIMIT 6")
    return render_template("admin/overview.html", n=n, by_status=by_status, vers=vers, games=games,
                           synced=_synced(), beta=releases.manifest("beta"), stable=releases.manifest("stable"),
                           charts=charts, sparks=sparks, deltas=deltas, totals=totals, recent=recent,
                           pending=_pending_promotions(), blocked=_blocked())


def _blocked():
    """Chaves derrubadas por uso em 2 consoles, esperando o admin."""
    return db.rows("SELECT u.id, u.name, k.console_id, k.dup_console, k.dup_at FROM beta_keys k JOIN users u ON u.id=k.user_id "
                   "WHERE k.revoked_at IS NULL AND k.dup_at IS NOT NULL AND u.active ORDER BY k.dup_at DESC LIMIT 20")


# ------------------------------------------------------------------ testadores
def _testers_ctx(**extra):
    q = request.args.get("q", "").strip()[:80]
    st = request.args.get("status", "")
    where, args = ["u.wait <> 'waiting'"], []   # quem está na lista de espera fica na página dela
    if q:
        where.append("(u.name ILIKE %s OR u.email ILIKE %s OR u.handle ILIKE %s"
                     " OR EXISTS (SELECT 1 FROM invites i WHERE i.used_by=u.id AND i.note ILIKE %s))")
        args += [_like(q)] * 4
    if st == "active":
        where.append("u.active")
    elif st == "inactive":
        where.append("NOT u.active")
    elif st == "nokey":
        where.append("u.active AND k.key_hint IS NULL")
    elif st == "blocked":
        where.append("u.active AND k.dup_at IS NOT NULL")
    else:
        st = ""
    base = """FROM users u LEFT JOIN LATERAL (SELECT key_hint, created_at, console_id, dup_at FROM beta_keys
                 WHERE user_id=u.id AND revoked_at IS NULL ORDER BY id DESC LIMIT 1) k ON true WHERE """ + " AND ".join(where)
    total = db.row("SELECT count(*) AS n " + base, *args)["n"]
    pg = ui.Page(total, ui.page_arg(request), PER_PAGE)
    us = db.rows("""SELECT u.*, k.key_hint, k.created_at AS key_at, k.console_id AS key_console, k.dup_at AS key_dup_at,
                    (SELECT i.note FROM invites i WHERE i.used_by=u.id AND i.note<>'' ORDER BY i.id DESC LIMIT 1) AS inv_note,
                    (SELECT count(*) FROM reports r WHERE r.user_id=u.id) AS nrep,
                    (SELECT count(*) FROM reports r WHERE r.user_id=u.id AND r.status = ANY(%s)) AS nopen,
                    GREATEST(u.last_login_at, (SELECT max(last_seen) FROM sessions s WHERE s.user_id=u.id),
                             (SELECT max(rx) FROM log_index l WHERE (l.tester=u.handle AND u.handle <> '')
                                                                 OR (l.tester=%s AND u.id=%s))) AS last_seen
                    """ + base + " ORDER BY u.role, u.active DESC, u.created_at DESC LIMIT %s OFFSET %s",
                 list(OPEN_STATUSES), DEV_TESTER, (_dev_owner() or {}).get("id"), *args, pg.per, pg.offset)
    for u in us:
        u["key_mask"] = ("•••••-•••" + u["key_hint"]) if u["key_hint"] else None
    counts = db.row("""SELECT count(*) AS all, count(*) FILTER (WHERE active) AS active,
                       count(*) FILTER (WHERE NOT active) AS inactive,
                       count(*) FILTER (WHERE active AND NOT EXISTS (SELECT 1 FROM beta_keys k WHERE k.user_id=users.id
                                        AND k.revoked_at IS NULL)) AS nokey,
                       count(*) FILTER (WHERE active AND EXISTS (SELECT 1 FROM beta_keys k WHERE k.user_id=users.id
                                        AND k.revoked_at IS NULL AND k.dup_at IS NOT NULL)) AS blocked FROM users WHERE wait <> 'waiting'""")
    return dict(users=us, page=pg, q=q, st=st, counts=counts, **extra)


@bp.get("/testers")
def testers():
    return render_template("admin/testers.html", **_testers_ctx())


def _tester(uid):
    u = db.row("SELECT * FROM users WHERE id=%s", uid)
    if not u:
        abort(404)
    return u


@bp.post("/testers/<int:uid>/active")
def tester_active(uid):
    u = _tester(uid)
    if u["id"] == g.user["id"]:
        abort(400)
    on = request.form.get("on") == "1"
    db.run("UPDATE users SET active=%s WHERE id=%s", on, uid)
    if not on:
        sec.kill_sessions(uid)
        bridgekeys.revoke_user_keys(uid)   # reativar exige gerar chave nova
    sec.audit("user.active" if on else "user.deactivate", u["handle"])
    flash(_("adm.activated" if on else "adm.deactivated", n=u["name"]))
    return redirect(url_for("admin.testers"))


def _tester_ctx(u, **extra):
    key = bridgekeys.current(u["id"])
    events = db.rows("SELECT e.*, b.name AS by_name FROM key_events e LEFT JOIN users b ON b.id=e.by_user "
                     "WHERE e.user_id=%s ORDER BY e.at DESC, e.id DESC LIMIT 30", u["id"])
    hs = _log_handles(u)
    stats = db.row("SELECT (SELECT count(*) FROM reports WHERE user_id=%s) AS nrep, "
                   "(SELECT count(*) FROM log_index WHERE tester = ANY(%s) AND tester <> '') AS nlogs, "
                   "(SELECT max(rx) FROM log_index WHERE tester = ANY(%s) AND tester <> '') AS last_log",
                   u["id"], hs, hs)
    inv = db.row("SELECT note FROM invites WHERE used_by=%s AND note<>'' ORDER BY id DESC LIMIT 1", u["id"])
    return dict(t=u, key=key, events=events, stats=stats, inv_note=inv["note"] if inv else "", **extra)


@bp.get("/testers/<int:uid>")
def tester_detail(uid):
    return render_template("admin/tester_detail.html", **_tester_ctx(_tester(uid)))


@bp.post("/testers/<int:uid>/key")
def tester_key(uid):
    u = _tester(uid)
    detail = request.form.get("back") == "detail"
    if request.form.get("op") == "revoke":
        bridgekeys.revoke_user_keys(uid)
        sec.audit("key.revoke", u["handle"])
        flash(_("adm.key_revoked"))
        return redirect(url_for("admin.tester_detail", uid=uid) if detail else url_for("admin.testers"))
    if not u["active"]:
        abort(400)
    # o testador também vê esta chave uma vez na área dele (fica só cifrada até lá)
    k = bridgekeys.new_key(u, by_admin=g.user)
    sec.audit("key.regen", u["handle"])
    # a chave em claro só aparece nesta resposta (não vai pra cookie/flash)
    if detail:
        return render_template("admin/tester_detail.html", **_tester_ctx(u, new_key=k, new_key_for=u["name"]))
    return render_template("admin/testers.html", **_testers_ctx(new_key=k, new_key_for=u["name"]))


@bp.post("/testers/<int:uid>/unbind")
def tester_unbind(uid):
    """Troca legítima de console: a mesma chave volta a valer pro próximo console que usar."""
    u = _tester(uid)
    if not u["active"] or not bridgekeys.unbind(uid, g.user):
        abort(400)
    sec.audit("key.unbind", u["handle"])
    flash(_("key.unbound", n=u["name"]))
    return redirect(url_for("admin.tester_detail", uid=uid))


# ------------------------------------------------------------------ convites
INVITE_STATE = """CASE WHEN i.revoked_at IS NOT NULL THEN 'revoked' WHEN i.used_at IS NOT NULL THEN 'used'
                       WHEN i.expires_at <= now() THEN 'expired' ELSE 'pending' END"""


@bp.route("/invites", methods=["GET", "POST"])
def invites():
    link = None
    if request.method == "POST":
        try:
            amount = int(request.form.get("amount", "0"))
        except ValueError:
            amount = 0
        hours = amount * (24 if request.form.get("unit") == "days" else 1)
        if not 1 <= hours <= current_app.config["INVITE_MAX_HOURS"]:
            flash(_("adm.invite_bad"))
            return redirect(url_for("admin.invites"))
        tok = sec.token()
        db.run("INSERT INTO invites(token_sha256, note, created_by, expires_at) "
               "VALUES (%s,%s,%s, now() + make_interval(hours => %s))",
               sec.sha256(tok), (request.form.get("note") or "").strip()[:120], g.user["id"], hours)
        sec.audit("invite.create", "%dh" % hours)
        link = current_app.config["BASE_URL"] + url_for("auth.signup", tok=tok)
    st = request.args.get("state", "")
    if st not in ("pending", "used", "expired", "revoked"):
        st = ""
    where, args = ("WHERE " + INVITE_STATE + " = %s", [st]) if st else ("", [])
    total = db.row("SELECT count(*) AS n FROM invites i " + where, *args)["n"]
    pg = ui.Page(total, ui.page_arg(request), PER_PAGE)
    rows = db.rows("SELECT i.*, u.name AS used_name, c.name AS by_name, " + INVITE_STATE + " AS state "
                   "FROM invites i LEFT JOIN users u ON u.id=i.used_by LEFT JOIN users c ON c.id=i.created_by "
                   + where + " ORDER BY i.id DESC LIMIT %s OFFSET %s", *args, pg.per, pg.offset)
    counts = {r["state"]: r["n"] for r in db.rows("SELECT " + INVITE_STATE + " AS state, count(*) AS n FROM invites i GROUP BY 1")}
    counts["all"] = sum(counts.values())
    return render_template("admin/invites.html", invites=rows, link=link, page=pg, st=st, counts=counts,
                           max_days=current_app.config["INVITE_MAX_HOURS"] // 24)


@bp.post("/invites/<int:iid>/revoke")
def invite_revoke(iid):
    if db.run("UPDATE invites SET revoked_at=now() WHERE id=%s AND used_at IS NULL AND revoked_at IS NULL", iid):
        flash(_("adm.invite_revoked"))
    sec.audit("invite.revoke", iid)
    return redirect(url_for("admin.invites"))


# ------------------------------------------------------------------ relatos
@bp.get("/reports")
def reports():
    fv, fg, fs, fq, ft = (request.args.get(k, "").strip()[:80] for k in ("version", "game", "status", "q", "tester"))
    where, args = ["true"], []
    tu = None
    if ft:
        tu = _find_user(ft)
        if tu:
            where.append("r.user_id=%s"); args.append(tu["id"])
            ft = str(tu["id"])
        else:
            where.append("false")   # testador que não existe: lista vazia, nunca a geral
    if fv:
        where.append("r.version=%s"); args.append(fv)
    if fg:
        where.append("r.game ILIKE %s"); args.append(_like(fg))
    if fq:
        where.append("(r.description ILIKE %s OR u.name ILIKE %s OR r.console_id ILIKE %s OR r.id::text = %s"
                     " OR EXISTS (SELECT 1 FROM invites i WHERE i.used_by=u.id AND i.note ILIKE %s))")
        args += [_like(fq)] * 3 + [fq.lstrip("#"), _like(fq)]
    status_where = list(where)
    if fs == "open":
        where.append("r.status = ANY(%s)"); args.append(list(OPEN_STATUSES))
    elif fs in STATUSES:
        where.append("r.status=%s"); args.append(fs)
    else:
        fs = ""
    sql_from = " FROM reports r JOIN users u ON u.id=r.user_id WHERE "
    total = db.row("SELECT count(*) AS n" + sql_from + " AND ".join(where), *args)["n"]
    pg = ui.Page(total, ui.page_arg(request), PER_PAGE)
    reps = db.rows("SELECT r.*, u.name AS uname, u.handle, u.country, (SELECT i.note FROM invites i WHERE i.used_by=u.id AND i.note<>'' ORDER BY i.id DESC LIMIT 1) AS inv_note, "
                   "(SELECT count(*) FROM report_files f WHERE f.report_id=r.id) AS nfiles"
                   + sql_from + " AND ".join(where) + " ORDER BY r.id DESC LIMIT %s OFFSET %s", *args, pg.per, pg.offset)
    nargs = args[:len(args) - (1 if fs else 0)]
    counts = {r["status"]: r["n"] for r in db.rows("SELECT r.status, count(*) AS n" + sql_from + " AND ".join(status_where)
                                                    + " GROUP BY 1", *nargs)}
    counts["all"] = sum(counts.values())
    counts["open"] = sum(counts.get(s, 0) for s in OPEN_STATUSES)
    versions = [r["version"] for r in db.rows("SELECT DISTINCT version FROM reports WHERE version<>'' ORDER BY 1 DESC")]
    testers = db.rows("SELECT u.id, u.name, count(*) AS n FROM reports r JOIN users u ON u.id=r.user_id "
                      "GROUP BY u.id, u.name ORDER BY u.name")
    if tu and not any(t["id"] == tu["id"] for t in testers):
        testers.append({"id": tu["id"], "name": tu["name"], "n": 0})
    return render_template("admin/reports.html", reports=reps, versions=versions, statuses=STATUSES, testers=testers,
                           fv=fv, fg=fg, fs=fs, fq=fq, ft=ft, tu=tu, page=pg, counts=counts)


@bp.get("/reports/<int:rid>")
def report_detail(rid):
    r = db.row("SELECT r.*, u.name AS uname, u.handle, u.email, u.country, (SELECT i.note FROM invites i WHERE i.used_by=u.id AND i.note<>'' ORDER BY i.id DESC LIMIT 1) AS inv_note FROM reports r JOIN users u ON u.id=r.user_id "
               "WHERE r.id=%s", rid)
    if not r:
        abort(404)
    files = db.rows("SELECT * FROM report_files WHERE report_id=%s ORDER BY id", rid)
    events = db.rows("SELECT e.*, u.name AS uname FROM report_events e LEFT JOIN users u ON u.id=e.user_id "
                     "WHERE e.report_id=%s ORDER BY e.at, e.id", rid)
    console = re.sub(r"[^0-9a-f]", "", (r["console_id"] or "").lower())
    if console:
        logs = db.rows("SELECT * FROM log_index WHERE console LIKE %s AND day BETWEEN %s AND %s "
                       "ORDER BY day DESC, r2_key DESC LIMIT 12", console + "%",
                       r["created_at"].date() - timedelta(days=2), r["created_at"].date() + timedelta(days=1))
    else:
        logs = db.rows("SELECT * FROM log_index WHERE tester=%s AND day BETWEEN %s AND %s "
                       "ORDER BY day DESC, r2_key DESC LIMIT 12", r["handle"],
                       r["created_at"].date() - timedelta(days=2), r["created_at"].date() + timedelta(days=1))
    others = db.rows("SELECT id, status, description, created_at FROM reports WHERE user_id=%s AND id<>%s "
                     "ORDER BY id DESC LIMIT 5", r["user_id"], rid)
    back = request.args.get("back")
    return render_template("admin/report_detail.html", r=r, files=files, events=events, logs=logs, others=others,
                           statuses=STATUSES, back=back if sec.safe_next(back) else url_for("admin.reports"))


@bp.post("/reports/<int:rid>")
def report_update(rid):
    st = request.form.get("status")
    if st not in STATUSES:
        abort(400)
    note = (request.form.get("note") or "").strip()[:2000]
    old = db.row("SELECT status, admin_note FROM reports WHERE id=%s", rid)
    if not old:
        abort(404)
    with db.conn() as c:
        c.execute("UPDATE reports SET status=%s, admin_note=%s, updated_at=now() WHERE id=%s", (st, note, rid))
        if old["status"] != st or old["admin_note"] != note:
            c.execute("INSERT INTO report_events(report_id, user_id, status, note) VALUES (%s,%s,%s,%s)",
                      (rid, g.user["id"], st, note if note != old["admin_note"] else ""))
    sec.audit("report.update", "%d %s" % (rid, st))
    flash(_("adm.report_saved", n=rid))
    return redirect(request.form.get("back") if sec.safe_next(request.form.get("back")) else url_for("admin.reports"))


# ------------------------------------------------------------------ logs
@bp.get("/logs")
def logs():
    day, console, tester, app_v = (request.args.get(k, "").strip()[:64] for k in ("day", "console", "tester", "app"))
    where, args = ["true"], []
    if DAY_RE.match(day):
        where.append("day=%s"); args.append(day)
    else:
        day = ""
    if console:
        console = re.sub(r"[^0-9a-f]", "", console.lower())
        where.append("console LIKE %s"); args.append(console + "%")
    tu = None
    if tester:
        tu = _find_user(tester)
        if tu:   # o admin dono também leva os lotes da chave dev (tester="dev")
            where.append("tester = ANY(%s)"); args.append(_log_handles(tu))
            tester = tu["handle"]
        else:
            where.append("tester=%s"); args.append(tester)
    if app_v:
        where.append("app=%s"); args.append(app_v)
    total = db.row("SELECT count(*) AS n FROM log_index WHERE " + " AND ".join(where), *args)["n"]
    pg = ui.Page(total, ui.page_arg(request), 50)
    items = db.rows("SELECT * FROM log_index WHERE " + " AND ".join(where) + " ORDER BY day DESC, r2_key DESC "
                    "LIMIT %s OFFSET %s", *args, pg.per, pg.offset)
    days = db.rows("SELECT day, count(*) AS n, count(DISTINCT console) AS consoles FROM log_index "
                   "GROUP BY day ORDER BY day DESC LIMIT 14")
    owner = _dev_owner()
    dev_as = owner["handle"] if owner else DEV_TESTER
    testers = db.rows("SELECT CASE WHEN tester=%s THEN %s ELSE tester END AS tester, count(*) AS n, max(day) AS last "
                      "FROM log_index WHERE tester<>'' GROUP BY 1 ORDER BY max(day) DESC, 1 LIMIT 30", DEV_TESTER, dev_as)
    if tester and not any(t["tester"] == tester for t in testers):
        testers.append({"tester": tester, "n": 0, "last": None})   # testador sem logs continua selecionado
    consoles = db.rows("SELECT console, count(*) AS n, max(day) AS last, max(tester) AS tester FROM log_index "
                       "GROUP BY console ORDER BY max(day) DESC, console LIMIT 15")
    apps = [r["app"] for r in db.rows("SELECT DISTINCT app FROM log_index WHERE app NOT IN ('', '?') ORDER BY 1 DESC LIMIT 30")]
    names = _log_names()
    return render_template("admin/logs.html", items=items, days=days, testers=testers, consoles=consoles, apps=apps,
                           names=names, page=pg, q=dict(day=day, console=console, tester=tester, app=app_v), tu=tu,
                           synced=_synced())


@bp.post("/logs/sync")
def logs_sync():
    try:
        n = logindex.sync(_storage())
        flash(_("adm.synced_n", n=max(n, 0)))
    except Exception as e:
        current_app.logger.warning("sync manual falhou: %s", e)
        flash(_("adm.r2_error"), "error")
    return redirect(url_for("admin.logs"))


def _level(line):
    if ERR_RE.search(line):
        return "err"
    if WARN_RE.search(line):
        return "warn"
    return ""


@bp.get("/logs/view")
def log_view():
    key = request.args.get("key", "")
    if not logindex.KEY_RE.match(key):
        abort(404)
    if request.args.get("raw"):
        data = _storage().get(key)
        if data is None:
            abort(404)
        r = Response(data, mimetype="application/gzip")
        r.headers["Content-Disposition"] = "attachment; filename=" + key.replace("/", "_")
        return r
    got = logindex.read_batch(_storage(), key)
    if not got:
        abort(404)
    doc, size = got
    lines = [(str(l), _level(str(l))) for l in (doc.get("lines") or [])][:20000]
    meta = {k: doc.get(k) for k in ("rx", "console", "app", "game", "tester")}
    nerr = sum(1 for _l, lv in lines if lv == "err")
    nwarn = sum(1 for _l, lv in lines if lv == "warn")
    idx = db.row("SELECT * FROM log_index WHERE r2_key=%s", key)
    sib = []
    if idx:
        sib = db.rows("SELECT r2_key, rx, nlines FROM log_index WHERE console=%s AND day=%s ORDER BY r2_key DESC LIMIT 20",
                      idx["console"], idx["day"])
    names = _log_names()
    return render_template("admin/log_view.html", key=key, meta=meta, lines=lines, size=size, nerr=nerr, nwarn=nwarn,
                           siblings=sib, names=names, day=key.split("/")[1])


# ------------------------------------------------------------------ versões
@bp.get("/versions")
def versions():
    try:
        vs = releases.versions()
        err = False
    except Exception:
        vs, err = [], True
    pending = _pending_promotions()
    return render_template("admin/versions.html", versions=vs, beta=releases.manifest("beta"),
                           stable=releases.manifest("stable"), pending=pending,
                           pending_versions={p["version"] for p in pending}, r2_error=err,
                           semver=releases.semver_key)


@bp.post("/versions/promote")
def promote():
    v = request.form.get("version", "")
    if not releases.VER_RE.match(v) or v not in [m["version"] for m in releases.versions()]:
        abort(400)
    if not db.row("SELECT 1 FROM promotion_requests WHERE version=%s AND done_at IS NULL", v):
        db.run("INSERT INTO promotion_requests(version, requested_by) VALUES (%s,%s)", v, g.user["id"])
    sec.audit("version.promote_request", v)
    flash(_("adm.promote_flash", v=v))
    return redirect(url_for("admin.versions") + "#pending")


# ------------------------------------------------------------------ status público: recursos
LANG_SUF = ("pt", "en", "es")
FEATURE_COLS = ["grp", "status", "since_version", "sort"] + ["%s_%s" % (a, b) for a in ("name", "desc") for b in LANG_SUF]


def _feature_form():
    f = {k: (request.form.get(k) or "").strip() for k in FEATURE_COLS}
    errs = {}
    if f["grp"] not in ui.FEATURE_GROUPS:
        errs["grp"] = _("ft.err_grp")
    if f["status"] not in ui.FEATURE_STATUSES:
        errs["status"] = _("ft.err_status")
    if not 1 <= len(f["name_pt"]) <= 120:
        errs["name_pt"] = _("ft.err_name")
    for b in ("en", "es"):
        if len(f["name_" + b]) > 120:
            errs["name_" + b] = _("ft.err_name")
    for b in LANG_SUF:
        if len(f["desc_" + b]) > 300:
            errs["desc_" + b] = _("ft.err_desc")
    if f["since_version"] and not releases.VER_RE.match(f["since_version"]):
        errs["since_version"] = _("ft.err_version")
    try:
        f["sort"] = int(f["sort"] or 100)
    except ValueError:
        f["sort"] = 100
    return f, errs


@bp.get("/features")
def features():
    st, grp = request.args.get("status", ""), request.args.get("group", "")
    rows = db.rows("SELECT f.*, (SELECT count(*) FROM release_note_items i WHERE i.feature_id=f.id) AS nnotes "
                   "FROM features f ORDER BY sort, id")
    counts = {s: sum(1 for f in rows if f["status"] == s) for s in ui.FEATURE_STATUSES}
    counts["all"] = len(rows)
    rows = [f for f in rows if (st not in ui.FEATURE_STATUSES or f["status"] == st)
            and (grp not in ui.FEATURE_GROUPS or f["grp"] == grp)]
    groups = [(g_, [f for f in rows if f["grp"] == g_]) for g_ in ui.FEATURE_GROUPS]
    return render_template("admin/features.html", groups=[x for x in groups if x[1]], counts=counts,
                           st=st if st in ui.FEATURE_STATUSES else "", grp=grp if grp in ui.FEATURE_GROUPS else "",
                           statuses=ui.FEATURE_STATUSES, group_keys=ui.FEATURE_GROUPS)


@bp.route("/features/new", methods=["GET", "POST"])
@bp.route("/features/<int:fid>", methods=["GET", "POST"])
def feature_edit(fid=None):
    row = None
    if fid:
        row = db.row("SELECT * FROM features WHERE id=%s", fid)
        if not row:
            abort(404)
    ctx = dict(row=row, statuses=ui.FEATURE_STATUSES, group_keys=ui.FEATURE_GROUPS)
    if request.method == "GET":
        f = dict(row) if row else {"grp": request.args.get("group", "controls"), "status": "soon", "sort": 100}
        return render_template("admin/feature_edit.html", f=f, ferr={}, **ctx)
    f, errs = _feature_form()
    if errs:
        return render_template("admin/feature_edit.html", f=f, ferr=errs, errors=list(errs.values()), **ctx), 400
    vals = [f[c] for c in FEATURE_COLS]
    if row:
        db.run("UPDATE features SET " + ", ".join(c + "=%s" for c in FEATURE_COLS) + ", updated_at=now() WHERE id=%s",
               *vals, fid)
        sec.audit("feature.update", fid)
    else:
        fid = db.row("INSERT INTO features(" + ", ".join(FEATURE_COLS) + ") VALUES ("
                     + ", ".join(["%s"] * len(FEATURE_COLS)) + ") RETURNING id", *vals)["id"]
        sec.audit("feature.create", fid)
    flash(_("ft.saved", n=f["name_pt"]))
    return redirect(url_for("admin.features", group=f["grp"]))


@bp.post("/features/<int:fid>/delete")
def feature_delete(fid):
    r = db.row("DELETE FROM features WHERE id=%s RETURNING name_pt", fid)
    if r:
        sec.audit("feature.delete", fid)
        flash(_("ft.deleted", n=r["name_pt"]))
    return redirect(url_for("admin.features"))


# ------------------------------------------------------------------ status público: notas de versão
def _note_form():
    f = {k: (request.form.get(k) or "").strip()
         for k in ("version", "channel", "released_on", "summary_pt", "summary_en", "summary_es")}
    f["published"] = request.form.get("published") == "1"
    errs = {}
    if not releases.VER_RE.match(f["version"]):
        errs["version"] = _("ft.err_version")
    if f["channel"] not in ("beta", "stable"):
        errs["channel"] = _("ft.err_status")
    if f["released_on"]:
        try:
            date.fromisoformat(f["released_on"])
        except ValueError:
            errs["released_on"] = _("rn.err_date")
    for b in LANG_SUF:
        if len(f["summary_" + b]) > 600:
            errs["summary_" + b] = _("ft.err_desc")
    return f, errs


@bp.get("/notes")
def notes():
    rows = db.rows("SELECT n.*, (SELECT count(*) FROM release_note_items i WHERE i.note_id=n.id) AS nitems "
                   "FROM release_notes n ORDER BY released_on DESC NULLS FIRST, id DESC")
    return render_template("admin/notes.html", notes=rows)


def _note_ctx(n, f=None, ferr=None, errors=None):
    items = db.rows("SELECT i.*, f.name_pt AS f_name, f.status AS f_status FROM release_note_items i "
                    "LEFT JOIN features f ON f.id=i.feature_id WHERE i.note_id=%s ORDER BY i.sort, i.id", n["id"]) if n else []
    feats = db.rows("SELECT id, grp, status, name_pt FROM features ORDER BY grp, sort, id")
    return dict(n=n, f=f or (dict(n) if n else {"channel": "beta"}), ferr=ferr or {}, errors=errors or [],
                items=items, feats=feats, kinds=ui.NOTE_KINDS, group_keys=ui.FEATURE_GROUPS)


@bp.route("/notes/new", methods=["GET", "POST"])
@bp.route("/notes/<int:nid>", methods=["GET", "POST"])
def note_edit(nid=None):
    n = None
    if nid:
        n = db.row("SELECT * FROM release_notes WHERE id=%s", nid)
        if not n:
            abort(404)
    if request.method == "GET":
        return render_template("admin/note_edit.html", **_note_ctx(n))
    f, errs = _note_form()
    if not errs:
        other = db.row("SELECT id FROM release_notes WHERE version=%s", f["version"])
        if other and (not n or other["id"] != n["id"]):
            errs["version"] = _("rn.err_exists")
    if errs:
        return render_template("admin/note_edit.html", **_note_ctx(n, f, errs, list(errs.values()))), 400
    vals = (f["version"], f["channel"], f["released_on"] or None, f["summary_pt"], f["summary_en"], f["summary_es"],
            f["published"])
    if n:
        db.run("UPDATE release_notes SET version=%s, channel=%s, released_on=%s, summary_pt=%s, summary_en=%s, "
               "summary_es=%s, published=%s, updated_at=now() WHERE id=%s", *vals, n["id"])
        nid = n["id"]
    else:
        nid = db.row("INSERT INTO release_notes(version, channel, released_on, summary_pt, summary_en, summary_es, "
                     "published) VALUES (%s,%s,%s,%s,%s,%s,%s) RETURNING id", *vals)["id"]
    sec.audit("note.save", f["version"])
    flash(_("rn.saved", v=f["version"]))
    return redirect(url_for("admin.note_edit", nid=nid))


@bp.post("/notes/<int:nid>/delete")
def note_delete(nid):
    r = db.row("DELETE FROM release_notes WHERE id=%s RETURNING version", nid)
    if r:
        sec.audit("note.delete", r["version"])
        flash(_("rn.deleted", v=r["version"]))
    return redirect(url_for("admin.notes"))


def _item_form():
    f = {k: (request.form.get(k) or "").strip() for k in ("kind", "text_pt", "text_en", "text_es", "feature_id", "sort")}
    ok = (f["kind"] in ui.NOTE_KINDS and 1 <= len(f["text_pt"]) <= 300
          and len(f["text_en"]) <= 300 and len(f["text_es"]) <= 300)
    try:
        f["feature_id"] = int(f["feature_id"]) if f["feature_id"] else None
        f["sort"] = int(f["sort"] or 100)
    except ValueError:
        ok = False
    if ok and f["feature_id"] and not db.row("SELECT 1 FROM features WHERE id=%s", f["feature_id"]):
        ok = False
    return f, ok


@bp.post("/notes/<int:nid>/items")
@bp.post("/notes/<int:nid>/items/<int:iid>")
def note_item(nid, iid=None):
    if not db.row("SELECT 1 FROM release_notes WHERE id=%s", nid):
        abort(404)
    f, ok = _item_form()
    if not ok:
        flash(_("rn.err_item"), "error")
        return redirect(url_for("admin.note_edit", nid=nid) + "#items")
    vals = (f["kind"], f["text_pt"], f["text_en"], f["text_es"], f["feature_id"], f["sort"])
    if iid:
        db.run("UPDATE release_note_items SET kind=%s, text_pt=%s, text_en=%s, text_es=%s, feature_id=%s, sort=%s "
               "WHERE id=%s AND note_id=%s", *vals, iid, nid)
    else:
        db.run("INSERT INTO release_note_items(kind, text_pt, text_en, text_es, feature_id, sort, note_id) "
               "VALUES (%s,%s,%s,%s,%s,%s,%s)", *vals, nid)
    db.run("UPDATE release_notes SET updated_at=now() WHERE id=%s", nid)
    flash(_("rn.item_saved"))
    return redirect(url_for("admin.note_edit", nid=nid) + "#items")


@bp.post("/notes/<int:nid>/items/<int:iid>/delete")
def note_item_delete(nid, iid):
    db.run("DELETE FROM release_note_items WHERE id=%s AND note_id=%s", iid, nid)
    flash(_("rn.item_deleted"))
    return redirect(url_for("admin.note_edit", nid=nid) + "#items")


# ------------------------------------------------------------------ /jogos (página pública): esconder jogo / relato
@bp.get("/games")
def games():
    from .public import game_stats
    gs, summary = game_stats()
    pending = db.row("SELECT count(*) AS n FROM log_index WHERE play_v < %s", plays.PARSER_V)["n"]
    return render_template("admin/games.html", games=gs, summary=summary, pending=pending)


@bp.post("/games/visibility")
def game_visibility():
    gkey = (request.form.get("gkey") or "").strip()[:120]
    if not gkey:
        abort(400)
    hidden = request.form.get("hidden") == "1"
    db.run("INSERT INTO game_flags(gkey, hidden) VALUES (%s,%s) "
           "ON CONFLICT (gkey) DO UPDATE SET hidden=excluded.hidden, updated_at=now()", gkey, hidden)
    sec.audit("game.hide" if hidden else "game.show", gkey)
    name = (request.form.get("name") or gkey)[:80]
    flash(_("gm.hidden_saved" if hidden else "gm.shown_saved", n=name))
    return redirect(url_for("admin.games"))


@bp.post("/reports/<int:rid>/public")
def report_public(rid):
    hidden = request.form.get("hidden") == "1"
    if not db.run("UPDATE reports SET public_hidden=%s WHERE id=%s", hidden, rid):
        abort(404)
    sec.audit("report.public", "%d %s" % (rid, "hide" if hidden else "show"))
    flash(_("gm.rep_hidden" if hidden else "gm.rep_visible"))
    return redirect(url_for("admin.report_detail", rid=rid))


# ------------------------------------------------------------------ lista de espera pública (waitlist.py)
@bp.get("/waitlist")
def waitlist_page():
    waitlist.expire()
    c = dict(waitlist.counts())
    c["done"] = db.row("SELECT count(*) AS n FROM users WHERE reddit <> '' AND wait = ''")["n"]
    released = db.rows("SELECT u.id, u.name, u.reddit, u.released_at, "
                       "u.released_at + make_interval(hours => %s) AS deadline, "
                       "EXISTS (SELECT 1 FROM beta_keys k WHERE k.user_id=u.id AND k.revoked_at IS NULL) AS has_key "
                       "FROM users u WHERE u.wait='released' ORDER BY u.released_at, u.id", waitlist.DEADLINE_H)
    waiting = db.rows("SELECT id, name, reddit, created_at FROM users WHERE wait='waiting' AND active "
                      "ORDER BY created_at DESC LIMIT 300")
    return render_template("admin/waitlist.html", c=c, released=released, waiting=waiting,
                           deadline_h=waitlist.DEADLINE_H, release_max=waitlist.RELEASE_MAX,
                           public_url=current_app.config["BASE_URL"] + url_for("auth.waitlist_signup"))


@bp.post("/waitlist/release")
def waitlist_release():
    try:
        n = int(request.form.get("n", ""))
    except ValueError:
        n = 0
    if not 1 <= n <= waitlist.RELEASE_MAX:
        flash(_("wl.release_bad"))
        return redirect(url_for("admin.waitlist_page"))
    ids = waitlist.release(n)
    sec.audit("waitlist.release", "%d de %d" % (len(ids), n))
    flash(_("wl.released_n", n=len(ids)))
    return redirect(url_for("admin.waitlist_page"))
