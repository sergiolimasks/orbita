import re

from flask import Blueprint, Response, abort, current_app, g, redirect, render_template, request, url_for

from .. import db, plays, releases, ui
from ..i18n import OPEN_STATUSES

bp = Blueprint("public", __name__)
COLOR_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")


@bp.get("/")
def home():
    soon = db.rows("SELECT * FROM features WHERE status='soon' ORDER BY sort, id")
    groups = [(grp, [f for f in soon if f["grp"] == grp]) for grp in ui.FEATURE_GROUPS]
    return render_template("home.html", stable=releases.manifest("stable"),
                           video=current_app.config["VIDEO_EMBED_URL"],
                           soon_groups=[x for x in groups if x[1]])


# ------------------------------------------------------------------ status público + notas de versão
def _note_items(note_ids):
    out = {}
    if note_ids:
        for it in db.rows("SELECT i.*, f.name_pt AS f_name_pt, f.name_en AS f_name_en, f.name_es AS f_name_es, f.status AS f_status "
                          "FROM release_note_items i LEFT JOIN features f ON f.id=i.feature_id "
                          "WHERE i.note_id = ANY(%s) ORDER BY i.sort, i.id", list(note_ids)):
            out.setdefault(it["note_id"], []).append(it)
    return out


@bp.get("/status")
def status():
    st = request.args.get("status", "")
    grp = request.args.get("group", "")
    feats = db.rows("SELECT * FROM features ORDER BY sort, id")
    counts = {s: sum(1 for f in feats if f["status"] == s) for s in ui.FEATURE_STATUSES}
    shown = [f for f in feats if (st not in ui.FEATURE_STATUSES or f["status"] == st)
             and (grp not in ui.FEATURE_GROUPS or f["grp"] == grp)]
    groups = [(g_, [f for f in shown if f["grp"] == g_]) for g_ in ui.FEATURE_GROUPS]
    notes = db.rows("SELECT * FROM release_notes WHERE published ORDER BY released_on DESC NULLS LAST, id DESC LIMIT 20")
    return render_template("public/status.html", groups=[x for x in groups if x[1]], counts=counts, total=len(feats),
                           st=st if st in ui.FEATURE_STATUSES else "", grp=grp if grp in ui.FEATURE_GROUPS else "",
                           notes=notes, items=_note_items([n["id"] for n in notes]),
                           statuses=ui.FEATURE_STATUSES, group_keys=ui.FEATURE_GROUPS)


@bp.get("/status/v/<version>")
def release_note(version):
    if not releases.VER_RE.match(version):
        abort(404)
    n = db.row("SELECT * FROM release_notes WHERE version=%s", version)
    is_admin = g.get("user") and g.user["role"] == "admin"
    if not n or (not n["published"] and not is_admin):
        abort(404)
    items = _note_items([n["id"]]).get(n["id"], [])
    others = db.rows("SELECT version, channel, released_on FROM release_notes WHERE published AND id<>%s "
                     "ORDER BY released_on DESC NULLS LAST, id DESC LIMIT 8", n["id"])
    return render_template("public/release.html", n=n, items=items, others=others, kinds=ui.NOTE_KINDS)


# ------------------------------------------------------------------ /jogos: progresso dos testes
PLAY_COLS = ("r2_key, day, rx, app, tester_h, title_id, game, continued, outcome, n_drops, n_errors, n_hangs, "
             "samples, ge55, hist, kick")


def game_stats():
    """(jogos, resumo) com os escondidos marcados (`hidden`). Nada de console/testador sai daqui, só contagens."""
    rows = db.rows("SELECT " + PLAY_COLS + " FROM play_sessions")
    hidden = {r["gkey"] for r in db.rows("SELECT gkey FROM game_flags WHERE hidden")}
    cur = (releases.manifest("beta") or {}).get("version")
    return plays.games(rows, hidden, current=cur)


@bp.get("/jogos")
def games():
    gs, summary = game_stats()
    gs = [x for x in gs if not x["hidden"]]
    known = {plays.norm_name(x["name"]): x["name"] for x in gs}
    reps = db.rows("SELECT id, game, version, status, admin_note, created_at, updated_at FROM reports "
                   "WHERE NOT public_hidden AND status<>'duplicate' ORDER BY updated_at DESC, id DESC LIMIT 60")
    bugs = [plays.public_report(r, known) for r in reps]
    bc = db.row("SELECT count(*) FILTER (WHERE status = ANY(%s)) AS open, count(*) FILTER (WHERE status='fixed') AS fixed, "
                "count(*) AS total FROM reports WHERE NOT public_hidden AND status<>'duplicate'", list(OPEN_STATUSES))
    return render_template("public/games.html", games=gs, summary=summary, bugs=bugs, bc=bc,
                           badges=plays.BADGES)   # versão atual: beta_now() (context processor)


@bp.get("/games")
def games_alias():
    return redirect(url_for("public.games"), 301)


@bp.get("/download/stable")
def download_stable():
    """Leva pro ELF que a ponte serve no canal estável (/v1/update/<ver>/xenon.elf)."""
    m = releases.manifest("stable")
    url = (m or {}).get("url", "")
    if not url.startswith("/v1/update/"):
        abort(404)
    return redirect(current_app.config["BASE_URL"] + url, 302)


@bp.get("/brand.css")
def brand_css():
    cols = current_app.config["BRAND"]["colors"]
    body = ":root{%s}\n" % "".join("--%s:%s;" % (k.replace("_", "-"), v)
                                   for k, v in cols.items() if COLOR_RE.match(v))
    r = Response(body, mimetype="text/css")
    r.headers["Cache-Control"] = "public, max-age=300"
    return r


@bp.get("/healthz")
def healthz():
    db.row("SELECT 1")
    return Response("ok\n", mimetype="text/plain")


@bp.get("/robots.txt")
def robots():
    return Response("User-agent: *\nDisallow: /admin\nDisallow: /area\nDisallow: /invite\nDisallow: /setup\n",
                    mimetype="text/plain")
