import json
import re, secrets
from datetime import timedelta

from flask import (Blueprint, Response, abort, current_app, flash, g, redirect, render_template, request,
                   url_for)

from .. import bridgekeys, db, images, releases, security as sec, waitlist
from ..i18n import OPEN_STATUSES, gettext as _

bp = Blueprint("tester", __name__)


@bp.before_request
def _waitlist_gate():
    """Lista de espera: quem ainda espera o sorteio só vê o aviso (sem chave, download nem relatos)."""
    u = g.get("user")
    if u and u.get("wait") == "waiting" and request.endpoint != "tester.area":
        return redirect(url_for("tester.area"))

VER_FIELD = re.compile(r"^[0-9A-Za-z.+-]{0,32}$")
CONSOLE_FIELD = re.compile(r"^[0-9A-Fa-f-]{0,64}$")
GAME_FIELD = re.compile(r"^[^\x00-\x1f<>]{0,80}$")


def _storage():
    return current_app.extensions["storage"]


def _key():
    return bridgekeys.current(g.user["id"])


def _files_by_report(reps):
    files = {}
    if reps:
        for f in db.rows("SELECT id, report_id, width, height FROM report_files WHERE report_id = ANY(%s) ORDER BY id",
                         [r["id"] for r in reps]):
            files.setdefault(f["report_id"], []).append(f)
    return files


TEST_BUILD_KEY = "tester-builds/%d/%s"   # xenon.elf + meta.json, postos pela equipe (tools/tester_build.py)


def _test_build(uid):
    """Versão de teste só deste testador (ou None). meta: {"version","note"}."""
    raw = _storage().get(TEST_BUILD_KEY % (uid, "meta.json"))
    if not raw:
        return None
    try:
        m = json.loads(raw)
    except ValueError:
        return None
    return m if isinstance(m, dict) and m.get("version") else None


def _area_ctx():
    """Início do testador: chave, download da beta, passos e relatos recentes."""
    reps = db.rows("SELECT * FROM reports WHERE user_id=%s ORDER BY id DESC LIMIT 3", g.user["id"])
    counts = db.row("SELECT count(*) AS n, count(*) FILTER (WHERE status = ANY(%s)) AS open, "
                    "count(*) FILTER (WHERE status='fixed') AS fixed FROM reports WHERE user_id=%s",
                    list(OPEN_STATUSES), g.user["id"])
    key = _key()
    u = g.user
    wl_deadline = (u["released_at"] + timedelta(hours=waitlist.DEADLINE_H)
                   if u.get("wait") == "released" and u.get("released_at") else None)
    return dict(key=key, key_blocked=bool(key and key["dup_at"]), beta=releases.manifest("beta"),
                key_plain=bridgekeys.reveal(u["id"]) if key and not key["dup_at"] else None,
                reports=reps, counts=counts, test_build=_test_build(g.user["id"]), wl_deadline=wl_deadline,
                key_limit=_keys_today(u["id"]) >= KEYS_PER_DAY)


KEYS_PER_DAY = 5


def _keys_today(uid):
    return db.row("SELECT count(*) AS n FROM beta_keys WHERE user_id=%s AND created_at > now() - interval '1 day'",
                  uid)["n"]


def _form_ctx(form=None, errors=None):
    beta = releases.manifest("beta")
    vers = [m["version"] for m in releases.versions()][:15]
    return dict(beta=beta, versions=vers, f=form or {}, errors=errors or [],
                max_files=current_app.config["MAX_FILES"],
                max_mb=current_app.config["MAX_FILE_BYTES"] // 1048576)


@bp.get("/area")
@sec.login_required
def area():
    if g.user.get("wait") == "waiting":
        return render_template("tester/waiting.html", deadline_h=waitlist.DEADLINE_H)
    # chave nova gerada pelo admin (ex.: depois de uma derrubada): aparece aqui uma vez só
    k = bridgekeys.take_pending(g.user["id"])
    return render_template("area.html", new_key=k, **_area_ctx())


@bp.post("/area/key")
@sec.login_required
def key_regen():
    """Gera outra chave (a antiga para de valer). A chave aparece só nesta resposta.
    Chave derrubada (usada em 2 consoles): só o admin libera."""
    if bridgekeys.is_blocked(g.user["id"]):
        sec.audit("key.self_regen_blocked", g.user["handle"])
        return render_template("area.html", **_area_ctx()), 403
    if _keys_today(g.user["id"]) >= KEYS_PER_DAY:
        # v0.21.37: em vez do 429 seco, a área explica o limite (e o pendrive que não grava)
        return render_template("area.html", **_area_ctx()), 429
    k = bridgekeys.new_key(g.user)
    sec.audit("key.self_regen", g.user["handle"])
    return render_template("area.html", new_key=k, **_area_ctx())


@bp.get("/area/report")
@sec.login_required
def report_form():
    return render_template("tester/report_form.html", **_form_ctx())


@bp.get("/area/reports")
@sec.login_required
def reports():
    st = request.args.get("status", "")
    where, args = ["user_id=%s"], [g.user["id"]]
    if st == "open":
        where.append("status = ANY(%s)"); args.append(list(OPEN_STATUSES))
    elif st == "closed":
        where.append("NOT (status = ANY(%s))"); args.append(list(OPEN_STATUSES))
    else:
        st = ""
    reps = db.rows("SELECT * FROM reports WHERE " + " AND ".join(where) + " ORDER BY id DESC LIMIT 100", *args)
    counts = db.row("SELECT count(*) AS all, count(*) FILTER (WHERE status = ANY(%s)) AS open, "
                    "count(*) FILTER (WHERE NOT (status = ANY(%s))) AS closed FROM reports WHERE user_id=%s",
                    list(OPEN_STATUSES), list(OPEN_STATUSES), g.user["id"])
    return render_template("tester/reports.html", reports=reps, files=_files_by_report(reps), st=st, counts=counts)


@bp.get("/area/reports/<int:rid>")
@sec.login_required
def report_detail(rid):
    r = db.row("SELECT * FROM reports WHERE id=%s AND user_id=%s", rid, g.user["id"])
    if not r:
        abort(404)
    files = db.rows("SELECT * FROM report_files WHERE report_id=%s ORDER BY id", rid)
    events = db.rows("SELECT status, note, at FROM report_events WHERE report_id=%s ORDER BY at, id", rid)
    return render_template("tester/report_detail.html", r=r, files=files, events=events)


@bp.post("/area/reports")
@sec.login_required
def report_new():
    cfg = current_app.config
    f = {k: (request.form.get(k) or "").strip() for k in ("description", "version", "console_id", "game")}
    errs = []
    if not 10 <= len(f["description"]) <= 5000:
        errs.append(_("rep.err_desc"))
    if not (VER_FIELD.match(f["version"]) and CONSOLE_FIELD.match(f["console_id"]) and GAME_FIELD.match(f["game"])):
        errs.append(_("rep.err_field"))
    uploads = [u for u in request.files.getlist("photos") if u and u.filename]
    if len(uploads) > cfg["MAX_FILES"]:
        errs.append(_("rep.err_count", n=cfg["MAX_FILES"]))
    processed = []
    if not errs:
        for up in uploads:
            data = up.read(cfg["MAX_FILE_BYTES"] + 1)
            if len(data) > cfg["MAX_FILE_BYTES"]:
                errs.append(_("rep.err_size", mb=cfg["MAX_FILE_BYTES"] // 1048576))
                break
            try:
                processed.append(images.process(data))
            except images.BadImage:
                errs.append(_("rep.err_type"))
                break
    if errs:
        return render_template("tester/report_form.html", **_form_ctx(f, errs)), 400
    if not f["console_id"]:   # v0.21.37: o console da chave mais recente usada
        k = db.row("SELECT console_id FROM beta_keys WHERE user_id=%s AND console_id <> '' "
                   "ORDER BY coalesce(last_used_at, bound_at) DESC NULLS LAST LIMIT 1", g.user["id"])
        f["console_id"] = (k or {}).get("console_id") or ""
    with db.conn() as c:
        rep = c.execute("INSERT INTO reports(user_id, version, console_id, game, description) "
                        "VALUES (%s,%s,%s,%s,%s) RETURNING id",
                        (g.user["id"], f["version"], f["console_id"].lower(), f["game"], f["description"])).fetchone()
        stored = []
        try:
            for full, thumb, w, h in processed:
                base = "reports/%d/%s" % (rep["id"], secrets.token_hex(8))
                _storage().put(base + ".jpg", full, "image/jpeg")
                stored.append(base + ".jpg")
                _storage().put(base + "_t.jpg", thumb, "image/jpeg")
                stored.append(base + "_t.jpg")
                c.execute("INSERT INTO report_files(report_id, r2_key, thumb_key, size, width, height) "
                          "VALUES (%s,%s,%s,%s,%s,%s)", (rep["id"], base + ".jpg", base + "_t.jpg", len(full), w, h))
        except IOError as e:
            current_app.logger.warning("upload R2 falhou: %s", e)
            for k in stored:
                try:
                    _storage().delete(k)
                except Exception:
                    pass
            c.rollback()
            return render_template("tester/report_form.html", **_form_ctx(f, [_("rep.err_store")])), 503
    flash(_("rep.ok", n=rep["id"]), "success")
    return redirect(url_for("tester.report_detail", rid=rep["id"]))


@bp.get("/media/report/<int:fid>")
@sec.login_required
def media(fid):
    f = db.row("SELECT f.*, r.user_id FROM report_files f JOIN reports r ON r.id=f.report_id WHERE f.id=%s", fid)
    if not f or (f["user_id"] != g.user["id"] and g.user["role"] != "admin"):
        abort(404)
    data = _storage().get(f["thumb_key"] if request.args.get("t") else f["r2_key"])
    if data is None:
        abort(404)
    r = Response(data, mimetype="image/jpeg")
    r.headers["Cache-Control"] = "private, max-age=3600"
    r.headers["Content-Disposition"] = "inline; filename=report-%d-%d.jpg" % (f["report_id"], fid)
    return r


@bp.get("/area/download/teste")
@sec.login_required
def download_test():
    """Versão de teste feita só pra este testador (R2 tester-builds/<id>/)."""
    m = _test_build(g.user["id"])
    data = _storage().get(TEST_BUILD_KEY % (g.user["id"], "xenon.elf")) if m else None
    if data is None:
        abort(404)
    r = Response(data, mimetype="application/octet-stream")
    r.headers["Content-Disposition"] = "attachment; filename=xenon.elf"
    return r


@bp.get("/area/download/beta")
@sec.login_required
def download_beta():
    """A beta não é pública na ponte (exige chave); aqui o site serve direto do R2 pra quem está logado."""
    m = releases.manifest("beta")
    key = releases.elf_key((m or {}).get("version"))
    data = _storage().get(key) if key else None
    if data is None:
        abort(404)
    r = Response(data, mimetype="application/octet-stream")
    r.headers["Content-Disposition"] = "attachment; filename=xenon.elf"
    r.headers["X-Checksum-SHA256"] = m.get("sha256", "")
    return r
