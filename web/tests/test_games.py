"""Página pública /jogos: parser das sessões, classificação, agregação, privacidade e painel."""
import gzip, json

from conftest import login, login_admin, make_user, post
from xc360site import db, logindex, plays


def stat(t, fps=58.9, skipped=0, kbps=1500, loss=0.0):
    return ("STAT t=%d dec_fps=%.1f disp_fps=%.1f dec_ms=1.4 skipped=%d backlog=0 loss_pct=%.2f lost=0 kbps=%d ver=0.21.0"
            % (t, fps, fps, skipped, loss, kbps))


PLAY = "[xn] PLAY FORTNITE (Fortnite) f2p: sessao no Xbox"


# ------------------------------------------------------------------ parser / classificador
def test_parse_ok_session_skips_warmup():
    lines = ["[menu] abrindo", PLAY, "[sess] STATE STREAMING Fortnite",
             stat(10, 20.0), stat(12, 30.0),                 # primeiros 4 s: fora
             stat(14, 59.0, kbps=1000), stat(16, 60.0, skipped=3, kbps=2000), stat(18, 50.0, skipped=5, kbps=3000),
             "[native] fim do jogo (1): rede: 1 AUs", "[sess] STATE IDLE Sessao encerrada"]
    (s,) = plays.parse(lines, "")
    assert s["title_id"] == "FORTNITE" and s["game"] == "Fortnite" and not s["continued"]
    assert s["outcome"] == "ok" and s["samples"] == 3 and s["ge55"] == 2
    assert s["fps_min"] == 50.0 and s["fps_avg"] == 56.3 and s["fps_med"] == 59.0
    assert s["skipped"] == 5 and s["kbps_avg"] == 2000
    assert json.loads(s["hist"]) == {"59": 1, "60": 1, "50": 1}


def test_parse_classifies_problems():
    drop = plays.parse([PLAY, stat(1), stat(3), stat(5), "[sess] queda no meio do jogo (1/3 em 2 min): re-sinalizando",
                        "[sess] STATE RECONNECTING Reconectando", stat(7, 10.0), stat(9, 10.0), stat(11, 58.0)])[0]
    assert drop["outcome"] == "drop" and drop["n_drops"] == 1
    assert drop["samples"] == 2              # depois da reconexão os ~4 s também ficam de fora
    err = plays.parse([PLAY, stat(1), stat(5), "[menu] vídeo perdido há 5096 ms no jogo: saindo pro menu",
                       "[sess] STATE ERROR Tempo esgotado esperando o xCloud (Expired)"])[0]
    assert err["outcome"] == "error" and err["n_drops"] == 1 and err["n_errors"] == 1
    hang = plays.parse([PLAY, stat(1), stat(5), "[wdog] TRAVOU: main parado há 10 s"])[0]
    assert hang["outcome"] == "hang"
    crash = plays.parse([PLAY, "[wdog] CRASH em 0x8001"])[0]
    assert crash["outcome"] == "hang" and crash["samples"] == 0
    # login que faltou não é problema do jogo; HANG.TXT aberto é só aviso
    nolog = plays.parse([PLAY, "[wdog] HANG.TXT aberto: contíguo", "[sess] STATE ERROR Faca o login no Xbox primeiro"])[0]
    assert nolog["outcome"] == "nostream" and nolog["n_errors"] == 0 and nolog["n_hangs"] == 0


def test_parse_multiple_sessions_and_continuation():
    lines = [stat(3000, 58.0), stat(3002, 59.0), "[sess] STATE ERROR Sem resposta do xCloud: HTTP 403",
             "[xn] PLAY CALLOFDUTYHQ: hw3 ainda e do decoder",          # não é sessão
             "[xn] PLAY GENSHINIMPACT (Genshin Impact): sessao no Xbox", stat(1), stat(3), stat(5, 40.0),
             "[xn] PLAY MORTALKOMBAT1 (Mortal Kombat 1): sessao no Xbox", "[sess] STATE IDLE Sessao encerrada"]
    segs = plays.parse(lines, "Fortnite")
    # v3: o lote abriu outros jogos depois, então o trecho do começo (de um jogo anterior) não herda o nome
    assert [(s["game"], s["continued"], s["outcome"]) for s in segs] == [
        ("Genshin Impact", False, "ok"), ("Mortal Kombat 1", False, "nostream")]
    assert segs[0]["samples"] == 1 and segs[0]["fps_min"] == 40.0
    # sem PLAY no lote, a continuação herda o nome e não pula aquecimento
    cont = plays.parse([stat(3000, 58.0), stat(3002, 59.0), "[sess] STATE ERROR Sem resposta do xCloud: HTTP 403"], "Fortnite")
    assert [(s["game"], s["continued"], s["outcome"], s["samples"]) for s in cont] == [("Fortnite", True, "error", 2)]
    # continuação sem nome de jogo nenhum: descartada
    assert plays.parse([stat(10), stat(12)], "") == []
    # nome pela linha STATE STREAMING quando o lote não tem "game"
    assert plays.parse(["[sess] STATE STREAMING Halo Infinite", stat(10)], "")[0]["game"] == "Halo Infinite"


def _row(game="Fortnite", tid="FORTNITE", fps=59, n=100, ge=None, outcome="ok", cont=False, who="a", app="v0.21.0",
         rx=None, key="k", drops=0, errors=0, hangs=0, hist=None):
    return {"r2_key": key, "day": None, "rx": rx, "app": app, "tester_h": who, "title_id": tid, "game": game,
            "continued": cont, "outcome": outcome, "n_drops": drops, "n_errors": errors, "n_hangs": hangs,
            "samples": n, "ge55": n if ge is None else ge,
            "hist": json.dumps(hist if hist is not None else ({str(fps): n} if n else {})), "kick": ""}


def test_badges_and_aggregation():
    rows = [_row(who="a"), _row(who="b", key="k2"), _row(who="a", key="k3", app="v0.21.1"),
            _row(game="Fortnite", tid="", cont=True, key="k4"),                     # continuação: não conta sessão
            _row(game="Slow", tid="SLOW", fps=40, ge=0, key="k5"),
            _row(game="Meh", tid="MEH", ge=80, key="k6", hist={"58": 80, "50": 20}),
            _row(game="Broke", tid="BROKE", n=0, outcome="error", errors=1, key="k7"),
            _row(game="Login", tid="LOGIN", n=0, outcome="nostream", key="k8"),
            _row(game="Hidden", tid="HID", key="k9", who="z")]
    gs, summary = plays.games(rows, hidden={"HID"})
    by = {g["name"]: g for g in gs}
    assert "Login" not in by
    f = by["Fortnite"]
    assert f["sessions"] == 3 and f["testers"] == 2 and f["fps_med"] == 59 and f["pct55"] == 100
    assert f["status"] == "good" and not f["few"]
    assert by["Slow"]["status"] == "bad" and by["Meh"]["status"] == "unstable" and by["Broke"]["status"] == "bad"
    assert by["Broke"]["fps_med"] is None and by["Broke"]["errors"] == 1
    assert by["Hidden"]["hidden"]
    assert gs[0]["name"] == "Fortnite"                         # ordenado por sessões
    assert summary["games"] == 4 and summary["sessions"] == 6 and summary["testers"] == 2
    assert summary["top_version"] == "0.21.0"
    assert by["Slow"]["target"] == 60 and f["target"] == 60
    # jogo que o xCloud manda a 30 fps: medido contra 30
    g30 = plays.games([_row(game="Crash", tid="CRASH", fps=30, ge=0, key="c%d" % i, who=str(i)) for i in range(3)])[0][0]
    assert g30["target"] == 30 and g30["pct55"] == 0 and g30["pct_ok"] == 100 and g30["status"] == "good"
    # continuação sem PLAY antes (lote do PLAY perdido / outro testador) conta como sessão; logo depois do PLAY, não
    from datetime import datetime, timedelta, timezone
    t0 = datetime(2026, 10, 26, 12, tzinfo=timezone.utc)
    cont = [_row(key="p1", rx=t0, who="a"), _row(key="p2", rx=t0 + timedelta(minutes=3), cont=True, tid="", who="a"),
            _row(key="p3", rx=t0 + timedelta(minutes=5), cont=True, tid="", who="b"),
            _row(key="p4", rx=t0 + timedelta(hours=9), cont=True, tid="", who="a")]
    assert plays.games(cont)[0][0]["sessions"] == 3
    # quedas demais tiram o "roda bem"
    many = [_row(key="d%d" % i, drops=1 if i < 2 else 0, outcome="drop" if i < 2 else "ok") for i in range(4)]
    assert plays.games(many)[0][0]["status"] == "unstable"


def test_current_version_rules():
    # erro de versão anterior à beta atual = corrigido; da atual (ou mais nova) continua
    rows = [_row(game="Old", tid="OLD", n=0, outcome="error", errors=1, app="v0.20.7", key="o1"),
            _row(game="Now", tid="NOW", n=0, outcome="error", errors=1, app="v0.21.29", key="n1")]
    by = {g["name"]: g for g in plays.games(rows, current="0.21.29")[0]}
    assert by["Old"]["errors"] == 0 and by["Now"]["errors"] == 1
    old_err_drop = [_row(game="X", tid="X", n=0, outcome="error", errors=1, drops=1, app="v0.20.7", key="x1")]
    assert plays.games(old_err_drop, current="0.21.29")[0][0]["drops"] == 0
    assert by["Old"]["old"] and not by["Now"]["old"]
    # mais sessões boas do que quedas: a queda sai; empate: fica
    more_ok = [_row(key="m%d" % i, who=str(i), drops=1 if i == 0 else 0, outcome="drop" if i == 0 else "ok") for i in range(3)]
    assert plays.games(more_ok, current="0.21.29")[0][0]["drops"] == 0
    tie = [_row(key="t%d" % i, who=str(i), drops=1 if i == 0 else 0, outcome="drop" if i == 0 else "ok") for i in range(2)]
    assert plays.games(tie, current="0.21.29")[0][0]["drops"] == 1


def _kick_line(reason):
    # formato real do log: o JSON do "content" vem escapado dentro da mensagem
    return ('[dc] recv [message sid=4 len=252] {"content":"{\\"reason\\":\\"%s\\"}","cv":"x",'
            '"target":"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect"}' % reason)


def test_server_kicks_are_not_drops():
    base = [PLAY] + [stat(t) for t in range(1, 40, 2)]
    # fim do tempo do gratuito: o vídeo perdido depois é consequência, não queda
    s = plays.parse(base + [_kick_line("KickForOutOfStreamingTime"), "[menu] vídeo perdido há 5006 ms no jogo"])[0]
    assert s["kick"] == "KickForOutOfStreamingTime" and s["outcome"] == "ok" and s["n_drops"] == 0
    # queda ANTES do motivo do servidor continua contando
    s = plays.parse(base + ["[sess] queda no meio do jogo (1/3 em 2 min): re-sinalizando",
                            _kick_line("KickForStopCommand"), "[menu] vídeo perdido há 5000 ms no jogo"])[0]
    assert s["n_drops"] == 1
    # conta aberta em outro aparelho
    s = plays.parse(base + ["[sess] keepalive falhou: HTTP 403 em keepalive (SessionOwnedByAnotherInstance)",
                            "[menu] vídeo perdido há 5000 ms no jogo"])[0]
    assert s["kick"] == "KickByNewSession" and s["n_drops"] == 0
    # jogo que fecha ao abrir: aparece como "não abre"
    closed = plays.parse(["[xn] PLAY GENSHINIMPACT (Genshin Impact): sessao no Xbox", stat(1), stat(3),
                          _kick_line("KickForClosedGame"), "[menu] vídeo perdido há 5000 ms no jogo"])[0]
    assert closed["kick"] == "KickForClosedGame" and closed["n_drops"] == 0
    rows = [dict(_row(game="Genshin Impact", tid="GENSHINIMPACT", n=0, outcome="nostream", key="g%d" % i, who=str(i)),
                 kick="KickForClosedGame") for i in range(3)]
    g = plays.games(rows, current="0.21.30")[0][0]
    assert g["status"] == "noopen" and g["drops"] == 0


def test_continuation_does_not_take_next_game_name():
    lines = [stat(t) for t in range(1, 30, 2)] + ["[xn] PLAY GENSHINIMPACT (Genshin Impact): sessao no Xbox", stat(40)]
    segs = plays.parse(lines, "Genshin Impact")
    assert [s["game"] for s in segs] == ["Genshin Impact"] and not segs[0]["continued"]
    # sem PLAY no lote, a continuação herda o nome do lote; "?" não é nome
    assert plays.parse([stat(t) for t in range(1, 9, 2)], "Fortnite")[0]["game"] == "Fortnite"
    assert plays.parse([stat(t) for t in range(1, 9, 2)], "?") == []


def test_scrub_and_public_report():
    t = plays.scrub("Fale com joao@example.com ou +55 11 91234-5678. Corrigido na 0.20.7 (2026-10-27).")
    assert "example.com" not in t and "91234" not in t and "0.20.7" in t and "2026-10-27" in t
    r = {"id": 3, "game": "fortnite ", "version": "0.20.7", "status": "fixed", "admin_note": "ok",
         "created_at": None, "updated_at": None, "description": "segredo"}
    p = plays.public_report(r, {"fortnite": "Fortnite"})
    assert p["game"] == "Fortnite" and p["version"] == "0.20.7" and "description" not in p
    assert plays.public_report(dict(r, game="Meu nome é Zé", version="<x>"), {})["game"] is None


# ------------------------------------------------------------------ indexador + página
CONSOLE = "abcdef0123456789"


def put_batch(app, name, lines, game="Fortnite", tester="h-t", app_v="v0.21.0", console=CONSOLE, day="2026-10-26"):
    key = "logs/%s/%s/%s.json.gz" % (day, console, name)
    doc = {"rx": day + "T12:00:00+00:00", "ip_h": "iphash99", "console": console, "app": app_v, "game": game,
           "tester": tester, "lines": lines}
    app.extensions["storage"].put(key, gzip.compress(json.dumps(doc).encode()))
    return key


def _good(n=10):
    return [PLAY] + [stat(2 * i, 59.0) for i in range(n)] + ["[native] fim do jogo (1)"]


def test_sync_stores_sessions_hashed(app):
    put_batch(app, "120000-aa", _good())
    put_batch(app, "120100-bb", [PLAY, stat(0), stat(2), stat(4), "[wdog] TRAVOU main"], tester="h-o",
              console="0123456789abcdef")
    put_batch(app, "120200-cc", ["nada de jogo"], game="")
    with app.app_context():
        assert logindex.sync(app.extensions["storage"]) == 3
    rows = db.rows("SELECT * FROM play_sessions ORDER BY r2_key")
    assert len(rows) == 2 and {r["outcome"] for r in rows} == {"ok", "hang"}
    for r in rows:
        assert CONSOLE not in r["console_h"] and r["console_h"] and r["tester_h"] not in ("h-t", "h-o")
    assert db.row("SELECT count(*) AS n FROM log_index WHERE play_v < %s", plays.PARSER_V)["n"] == 0


def test_backfill_existing_logs(app):
    key = put_batch(app, "120000-aa", _good())
    bad = "logs/2026-10-26/%s/120500-ee.json.gz" % CONSOLE
    app.extensions["storage"].put(bad, b"lixo")
    for k in (key, bad):    # índice antigo: lotes já indexados, ainda não lidos pelo parser de sessões
        db.run("INSERT INTO log_index(r2_key, day, console, app, game) VALUES (%s, '2026-10-26', %s, 'v0.21.0', 'Fortnite')",
               k, CONSOLE)
    db.run("INSERT INTO kv(k, v) VALUES ('logs_backfill', '1')")
    with app.app_context():
        assert logindex.backfill_all(app.extensions["storage"]) == 2
        assert logindex.backfill_all(app.extensions["storage"]) == 0
    assert db.row("SELECT count(*) AS n FROM play_sessions")["n"] == 1


def test_games_page_public_and_private_data_hidden(app, client):
    u = make_user(app, name="Fulano Secreto", email="fulano@example.com")
    put_batch(app, "120000-aa", _good())
    put_batch(app, "120100-bb", [PLAY, stat(0), stat(2), stat(4), "[sess] STATE ERROR Tempo esgotado (Expired)"])
    with app.app_context():
        logindex.sync(app.extensions["storage"])
    db.run("INSERT INTO reports(user_id, version, console_id, game, description, status, admin_note) VALUES "
           "(%s, '0.21.0', %s, 'Fortnite', 'Meu zap 11 91234-5678, tela preta', 'fixed', 'Corrigido na 0.21.1. Dúvidas: dev@example.com'), "
           "(%s, '0.21.0', '', 'Fortnite', 'escondido', 'confirmed', 'NAO-PUBLICAR'), "
           "(%s, '0.21.0', '', 'Fortnite', 'dup', 'duplicate', 'DUPLICADO-X')", u["id"], CONSOLE, u["id"], u["id"])
    db.run("UPDATE reports SET public_hidden=true WHERE admin_note='NAO-PUBLICAR'")
    r = client.get("/jogos?lang=pt-BR")
    html = r.get_data(as_text=True)
    assert r.status_code == 200
    assert "Progresso dos testes" in html and "Fortnite" in html and html.count("data-game-row") == 1
    assert "Bugs encontrados" in html and "Corrigido na 0.21.1" in html and html.count("data-bug-row") == 1
    for secret in (CONSOLE, "h-t", "Fulano", "fulano@example.com", "dev@example.com", "91234", "tela preta",
                   "iphash99", "NAO-PUBLICAR", "DUPLICADO-X", "escondido"):
        assert secret not in html, secret
    assert 'href="/jogos"' in client.get("/").get_data(as_text=True)          # link no topo
    assert "Test progress" in client.get("/jogos?lang=en").get_data(as_text=True)
    assert "Progreso de las pruebas" in client.get("/jogos?lang=es").get_data(as_text=True)
    assert client.get("/games").status_code == 301


def test_games_page_empty(client):
    html = client.get("/jogos?lang=en").get_data(as_text=True)
    assert "No game sessions recorded yet." in html and "No public reports yet." in html


def test_admin_hide_game_and_report(app, client):
    t = make_user(app)
    put_batch(app, "120000-aa", _good())
    with app.app_context():
        logindex.sync(app.extensions["storage"])
    rid = db.row("INSERT INTO reports(user_id, game, description, admin_note) VALUES (%s, 'Fortnite', 'x', 'RESPOSTA-1') "
                 "RETURNING id", t["id"])["id"]
    login_admin(client, app)
    html = client.get("/admin/games").get_data(as_text=True)
    assert "Fortnite" in html and html.count("data-admin-game") == 1
    post(client, "/admin/games/visibility", {"gkey": "FORTNITE", "hidden": "1", "name": "Fortnite"}, page="/admin/games")
    pub = app.test_client().get("/jogos?lang=en").get_data(as_text=True)
    assert "data-game-row" not in pub and "RESPOSTA-1" in pub
    assert "data-admin-game" in client.get("/admin/games").get_data(as_text=True)     # admin continua vendo
    post(client, "/admin/reports/%d/public" % rid, {"hidden": "1"}, page="/admin/reports/%d" % rid)
    assert db.row("SELECT public_hidden FROM reports WHERE id=%s", rid)["public_hidden"]
    assert "RESPOSTA-1" not in app.test_client().get("/jogos").get_data(as_text=True)
    post(client, "/admin/games/visibility", {"gkey": "FORTNITE", "hidden": "0"}, page="/admin/games")
    assert "data-game-row" in app.test_client().get("/jogos").get_data(as_text=True)
    # testador não acessa o painel
    tc = app.test_client()
    login(tc)
    assert tc.get("/admin/games").status_code in (302, 403, 404)
