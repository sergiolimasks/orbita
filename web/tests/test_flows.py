import gzip, io, json, re, time

import pyotp
from PIL import Image

from conftest import CON_A, CON_B, CON_C, bcheck, csrf, login, login_admin, make_user, new_invite, post
from xc360site import db, security

SIGNUP = {"name": "José da Silva", "email": "jose@example.com", "phone": "+55 11 91234-5678", "country": "BR",
          "password": "uma-senha-boa-1", "password2": "uma-senha-boa-1", "accept": "1"}


def signup(client, link, **over):
    path = link.replace("https://x360.test", "")
    data = dict(SIGNUP, **over)
    return post(client, path, data)


# ------------------------------------------------------------------ público / i18n / cabeçalhos
def test_home_public_and_soon(client):
    r = client.get("/", headers={"Accept-Language": "pt-BR,pt;q=0.9"})
    html = r.get_data(as_text=True)
    assert r.status_code == 200
    assert "Versão estável em breve" in html
    assert "orbita-horizontal-white.svg" in html and "Aviso de risco" in html
    assert "30 segundos" in html                      # FAQ tela vermelha
    assert "github.com/sergiolimasks/orbita" in html and "Todo o código é público" in html
    assert "Game Pass Ultimate" in html               # só como requisito, em texto


def test_home_stable_download(app, client):
    m = {"format": 1, "channel": "stable", "version": "1.2.3", "size": 10, "sha256": "ab" * 32,
         "url": "/v1/update/1.2.3/xenon.elf", "notes": "x", "sig": "s"}
    app.extensions["storage"].put("releases/stable.json", json.dumps(m).encode())
    html = client.get("/?lang=en").get_data(as_text=True)
    assert "Download stable version" in html and "Version 1.2.3" in html
    r = client.get("/download/stable")
    assert r.status_code == 302 and r.headers["Location"] == "https://x360.test/v1/update/1.2.3/xenon.elf"


def test_i18n_detect_and_switch(client):
    assert "Requisitos" in client.get("/", headers={"Accept-Language": "es-ES,es;q=0.9"}).get_data(as_text=True)
    assert "Instalación" in client.get("/", headers={"Accept-Language": "es"}).get_data(as_text=True)
    assert "Installation" in client.get("/", headers={"Accept-Language": "en-US"}).get_data(as_text=True)
    assert "Instalação" in client.get("/", headers={"Accept-Language": "pt-BR"}).get_data(as_text=True)
    r = client.get("/?lang=es", headers={"Accept-Language": "pt-BR"})
    assert "lang=es" in r.headers.get("Set-Cookie", "")
    assert "Instalación" in client.get("/", headers={"Accept-Language": "pt-BR"}).get_data(as_text=True)


def test_i18n_complete():
    from xc360site.i18n import M
    for k, v in M.items():
        assert len(v) == 3 and all(v), k
        if isinstance(v[0], list):
            assert len(v[0]) == len(v[1]) == len(v[2]), k


def test_countries_localized(client):
    from xc360site.countries import countries
    assert dict(countries("pt-BR"))["DE"] == "Alemanha"
    assert dict(countries("en"))["DE"] == "Germany"
    assert dict(countries("es"))["DE"] == "Alemania"
    assert len(countries("en")) > 240


def test_security_headers(client):
    r = client.get("/")
    h = r.headers
    assert "frame-ancestors 'none'" in h["Content-Security-Policy"]
    assert h["X-Content-Type-Options"] == "nosniff"
    assert "max-age" in h["Strict-Transport-Security"]
    assert h["X-Frame-Options"] == "DENY"


def test_csrf_required(client):
    r = client.post("/login", data={"email": "a@b.cd", "password": "x"})
    assert r.status_code == 400


def test_session_cookie_flags(app, client):
    make_user(app)
    r = login(client)
    c = r.headers["Set-Cookie"]
    assert "__Host-xc360=" in c and "HttpOnly" in c and "Secure" in c and "SameSite=Lax" in c


# ------------------------------------------------------------------ login / bloqueio / papéis
def test_login_ok_and_wrong(app, client):
    make_user(app)
    assert login(client, pw="errada-errada").status_code == 401
    r = login(client)
    assert r.status_code == 302 and r.headers["Location"].endswith("/area")
    assert client.get("/area").status_code == 200


def test_lockout_after_failures(app, client):
    make_user(app)
    for _ in range(5):
        assert login(client, pw="errada-errada").status_code == 401
    r = login(client)   # senha certa, mas conta bloqueada
    assert r.status_code == 429
    db.run("UPDATE users SET locked_until = now() - interval '1 second'")
    assert login(client).status_code == 302


def test_ip_rate_limit(app, client):
    app.config["IP_MAX_FAILS"] = 3
    try:
        for i in range(3):
            login(client, email="x%d@example.com" % i, pw="nada-nada-nada")
        assert login(client, email="y@example.com", pw="nada-nada-nada").status_code == 429
    finally:
        app.config["IP_MAX_FAILS"] = 20


def test_inactive_user_cannot_login(app, client):
    make_user(app, active=False)
    assert login(client).status_code == 401


def test_roles(app, client):
    assert client.get("/admin/").status_code == 302          # anônimo -> login
    assert client.get("/area").status_code == 302
    make_user(app)
    login(client)
    assert client.get("/admin/").status_code == 403          # testador
    assert client.get("/admin/testers").status_code == 403
    assert client.get("/area").status_code == 200


def test_logout_kills_session(app, client):
    make_user(app)
    login(client)
    post(client, "/logout", {}, page="/area")
    assert client.get("/area").status_code == 302
    assert db.row("SELECT count(*) AS n FROM sessions")["n"] == 0


# ------------------------------------------------------------------ 2FA
def test_admin_requires_totp(app, client):
    u = make_user(app, role="admin", email="admin@example.com", totp=True)
    r = login(client, "admin@example.com")
    assert r.headers["Location"].endswith("/login/2fa")
    assert client.get("/admin/").status_code == 302          # ainda não logado
    assert post(client, "/login/2fa", {"code": "000000"}).status_code == 401
    code = pyotp.TOTP(u["_totp"]).now()
    assert post(client, "/login/2fa", {"code": code}).status_code == 302
    assert client.get("/admin/").status_code == 200


def test_totp_code_cannot_be_replayed(app, client):
    u = make_user(app, role="admin", email="admin@example.com", totp=True)
    code = pyotp.TOTP(u["_totp"]).now()
    login(client, "admin@example.com")
    assert post(client, "/login/2fa", {"code": code}).status_code == 302
    c2 = app.test_client()
    login(c2, "admin@example.com")
    assert post(c2, "/login/2fa", {"code": code}).status_code == 401


def test_admin_setup_link_single_use(app, client):
    tok = security.token()
    db.run("INSERT INTO setup_links(token_sha256, expires_at) VALUES (%s, now() + interval '1 hour')", security.sha256(tok))
    r = post(client, "/setup/" + tok, {"name": "Dono", "email": "dono@example.com", "password": "senha-forte-123",
                                        "password2": "senha-forte-123"})
    assert r.status_code == 302 and r.headers["Location"].endswith("/2fa")
    html = client.get("/setup/%s/2fa" % tok).get_data(as_text=True)
    secret = re.search(r'id="totpkey">([A-Z2-7]+)<', html).group(1)
    assert "data:image/svg+xml;base64," in html
    assert login(client, "dono@example.com").status_code == 401  # ainda inativo
    assert post(client, "/setup/%s/2fa" % tok, {"code": "123456"}).status_code == 400
    r = post(client, "/setup/%s/2fa" % tok, {"code": pyotp.TOTP(secret).now()})
    assert r.status_code == 302 and r.headers["Location"].endswith("/admin/")
    assert client.get("/admin/").status_code == 200
    c2 = app.test_client()
    assert c2.get("/setup/" + tok).status_code == 410        # uso único


def test_cli_admin_link(app, monkeypatch, capsys):
    from xc360site import cli
    monkeypatch.setenv("DATABASE_URL", app.config["DATABASE_URL"])
    monkeypatch.setenv("BASE_URL", "https://x360.test")
    cli.main(["admin-link", "--hours", "2"])
    out = capsys.readouterr().out.strip()
    assert re.fullmatch(r"https://x360\.test/setup/[A-Za-z0-9_-]{40,}", out)
    from xc360site import db as d
    d.init(app.config["DATABASE_URL"])     # cli reiniciou o pool global


# ------------------------------------------------------------------ convites / cadastro
def test_invite_signup_flow(app, client):
    login_admin(client, app)
    r, link = new_invite(client)
    assert link and "/invite/" in link
    anon = app.test_client()
    r = signup(anon, link)
    assert r.status_code == 200
    html = r.get_data(as_text=True)
    key = re.search(r'id="betakey">([A-Z0-9-]+)<', html).group(1)
    assert re.fullmatch(r"[2-9A-HJKMNP-Z]{5}-[2-9A-HJKMNP-Z]{5}", key)
    u = db.row("SELECT * FROM users WHERE email='jose@example.com'")
    assert u["role"] == "tester" and u["country"] == "BR" and u["phone"].startswith("+55")
    k = db.row("SELECT * FROM bridge_beta_keys")
    assert k["key_sha256"] == security.sha256(key.replace("-", "")) and k["tester_name"] == u["handle"] and k["active"]
    # a chave em claro não fica no banco; a ponte valida pelo /_bridge/check
    assert key not in json.dumps([dict(r) for r in db.rows("SELECT * FROM beta_keys")], default=str)
    assert bcheck(anon, key) == "ok"
    # depois, na área, só aparece o final
    assert 'id="betakey">%s<' % key not in anon.get("/area").get_data(as_text=True)   # v0.21.38: só no Mostrar
    # uso único
    anon2 = app.test_client()
    assert anon2.get(link.replace("https://x360.test", "")).status_code == 410


def test_invite_expired_and_revoked(app, client):
    login_admin(client, app)
    _, link = new_invite(client, amount="1", unit="hours")
    db.run("UPDATE invites SET expires_at = now() - interval '1 second'")
    anon = app.test_client()
    assert anon.get(link.replace("https://x360.test", "")).status_code == 410
    _, link2 = new_invite(client)
    iid = db.row("SELECT id FROM invites ORDER BY id DESC LIMIT 1")["id"]
    post(client, "/admin/invites/%d/revoke" % iid, {}, page="/admin/invites")
    assert anon.get(link2.replace("https://x360.test", "")).status_code == 410


def test_invite_max_15_days(app, client):
    login_admin(client, app)
    r, link = new_invite(client, amount="16", unit="days")
    assert link is None and db.row("SELECT count(*) AS n FROM invites")["n"] == 0
    r, link = new_invite(client, amount="15", unit="days")
    assert link
    d = db.row("SELECT expires_at - created_at AS d FROM invites")["d"]
    assert d.days == 15


def test_signup_validation(app, client):
    login_admin(client, app)
    _, link = new_invite(client)
    anon = app.test_client()
    for over in ({"email": "nope"}, {"password": "curta", "password2": "curta"}, {"password2": "outra-senha-1"},
                 {"country": "XX"}, {"phone": "abc"}, {"accept": ""}, {"name": ""}):
        r = signup(anon, link, **over)
        assert r.status_code == 400, over
    assert db.row("SELECT count(*) AS n FROM users WHERE role='tester'")["n"] == 0
    make_user(app, email="jose@example.com")
    assert signup(anon, link).status_code == 400            # e-mail já existe
    assert db.row("SELECT used_at FROM invites")["used_at"] is None


# ------------------------------------------------------------------ chaves beta
def _newkey(r):
    return re.search(r'id="betakey">([A-Z0-9-]+)<', r.get_data(as_text=True)).group(1)


def test_key_regen_and_revoke(app, client):
    login_admin(client, app)
    t = make_user(app, email="t@example.com")
    k1 = _newkey(post(client, "/admin/testers/%d/key" % t["id"], {}, page="/admin/testers"))
    assert bcheck(client, k1) == "ok"
    k2 = _newkey(post(client, "/admin/testers/%d/key" % t["id"], {}, page="/admin/testers"))
    assert bcheck(client, k1) == "revoked" and bcheck(client, k2) == "ok"
    rows = db.rows("SELECT active FROM bridge_beta_keys ORDER BY key_sha256")
    assert sorted(r["active"] for r in rows) == [False, True]
    post(client, "/admin/testers/%d/key" % t["id"], {"op": "revoke"}, page="/admin/testers")
    assert bcheck(client, k2) == "revoked"
    assert bcheck(client, "ZZZZZ-ZZZZZ") == "invalid"
    assert not any(r["active"] for r in db.rows("SELECT active FROM bridge_beta_keys"))


def test_deactivate_revokes_key_and_session(app, client):
    login_admin(client, app)
    t = make_user(app, email="t@example.com")
    tc = app.test_client()
    login(tc)
    k = _newkey(post(tc, "/area/key", {}, page="/area"))
    assert bcheck(client, k) == "ok"
    post(client, "/admin/testers/%d/active" % t["id"], {"on": "0"}, page="/admin/testers")
    assert bcheck(client, k) == "revoked"
    assert tc.get("/area").status_code == 302


# ------------------------------------------------------------------ chave = um console
def test_bridge_endpoint_auth(app, client):
    t = make_user(app)
    with app.app_context():
        from xc360site import bridgekeys
        k = bridgekeys.new_key(t)
    assert bcheck(client, k, token=None) == 404                       # sem segredo
    assert bcheck(client, k, token="errado") == 404
    assert bcheck(client, k, headers={"X-Forwarded-For": "1.2.3.4"}) == 404   # veio pelo Traefik
    assert db.row("SELECT console_id FROM beta_keys")["console_id"] is None
    assert bcheck(client, k, console="xyz") == "invalid"               # console malformado
    assert bcheck(client, k, console="A" * 32) == "ok"                 # normaliza pra minúsculas
    assert db.row("SELECT console_id FROM beta_keys")["console_id"] == CON_A


def test_bind_first_use_then_duplicate_blocks_both(app, client):
    t = make_user(app)
    with app.app_context():
        from xc360site import bridgekeys
        k = bridgekeys.new_key(t)
    assert bcheck(client, k, CON_A) == "ok"                            # vincula no 1º uso
    row = db.row("SELECT console_id, bound_at, dup_at FROM beta_keys")
    assert row["console_id"] == CON_A and row["bound_at"] and row["dup_at"] is None
    assert bcheck(client, k.lower().replace("-", ""), CON_A) == "ok"   # mesmo console (chave digitada diferente)
    assert bcheck(client, k, CON_B, ip_h="beefbeef") == "duplicate"    # 2º console derruba
    assert bcheck(client, k, CON_A) == "duplicate"                     # os dois passam a receber 403
    assert bcheck(client, k, CON_B) == "duplicate"
    assert bcheck(client, k, CON_C) == "duplicate"
    row = db.row("SELECT console_id, dup_console, dup_ip_h, dup_at FROM beta_keys")
    assert row["console_id"] == CON_A and row["dup_console"] == CON_B and row["dup_ip_h"] == "beefbeef" and row["dup_at"]
    ev = db.rows("SELECT kind, console_a, console_b, ip_h, at FROM key_events ORDER BY id")
    assert [e["kind"] for e in ev] == ["bind", "duplicate"]            # um evento só, mesmo com tentativas depois
    assert (ev[1]["console_a"], ev[1]["console_b"], ev[1]["ip_h"]) == (CON_A, CON_B, "beefbeef") and ev[1]["at"]


def _dup_tester(app, client):
    """Admin logado em `client`, testador com chave derrubada (A, depois B)."""
    login_admin(client, app)
    t = make_user(app, email="t@example.com", name="Tester Um")
    k = _newkey(post(client, "/admin/testers/%d/key" % t["id"], {}, page="/admin/testers"))
    assert bcheck(client, k, CON_A) == "ok" and bcheck(client, k, CON_B) == "duplicate"
    return t, k


def test_admin_sees_blocked_key(app, client):
    t, k = _dup_tester(app, client)
    html = client.get("/admin/?lang=pt-BR").get_data(as_text=True)
    assert 'id="blocked-keys"' in html and "1 chave(s) bloqueada(s)" in html and "Tester Um" in html
    assert re.search(r'class="count hot">1<', html)                    # contador na sidebar
    html = client.get("/admin/testers?status=blocked").get_data(as_text=True)
    assert "Tester Um" in html and "/admin/testers/%d" % t["id"] in html
    html = client.get("/admin/testers/%d?lang=pt-BR" % t["id"]).get_data(as_text=True)
    assert 'id="dup-alert"' in html and CON_A in html and CON_B in html and "1234abcd" in html
    assert 'id="btn-newkey"' in html and 'id="btn-unbind"' in html
    for lang in ("en", "es"):
        assert client.get("/admin/testers/%d?lang=%s" % (t["id"], lang)).status_code == 200


def test_admin_unbind_lets_next_console(app, client):
    t, k = _dup_tester(app, client)
    r = post(client, "/admin/testers/%d/unbind" % t["id"], {}, page="/admin/testers/%d" % t["id"])
    assert r.status_code == 302
    row = db.row("SELECT console_id, dup_at, revoked_at FROM beta_keys WHERE revoked_at IS NULL")
    assert row["console_id"] is None and row["dup_at"] is None
    assert bcheck(client, k, CON_B) == "ok"                            # a mesma chave, agora no console novo
    assert db.row("SELECT console_id FROM beta_keys WHERE revoked_at IS NULL")["console_id"] == CON_B
    assert bcheck(client, k, CON_A) == "duplicate"                     # e o antigo derruba de novo
    kinds = [e["kind"] for e in db.rows("SELECT kind FROM key_events ORDER BY id")]
    assert kinds == ["new_by_admin", "bind", "duplicate", "unbind", "bind", "duplicate"]
    assert 'id="blocked-keys"' in client.get("/admin/").get_data(as_text=True)


def test_admin_new_key_after_duplicate_tester_sees_once(app, client):
    t, k = _dup_tester(app, client)
    tc = app.test_client()
    login(tc)
    tc.get("/area")                                                    # consome a 1ª chave (gerada antes da derrubada)
    r = post(client, "/admin/testers/%d/key" % t["id"], {"back": "detail"}, page="/admin/testers/%d" % t["id"])
    k2 = _newkey(r)
    assert k2 != k and 'id="dup-alert"' not in r.get_data(as_text=True)
    assert bcheck(client, k, CON_A) == "revoked" and bcheck(client, k, CON_B) == "revoked"
    assert k2 not in json.dumps([dict(x) for x in db.rows("SELECT * FROM beta_keys")], default=str)   # só cifrada
    html = tc.get("/area?lang=pt-BR").get_data(as_text=True)
    assert 'id="betakey">%s<' % k2 in html and 'id="key-blocked"' not in html
    assert 'id="betakey">%s<' % k2 not in tc.get("/area").get_data(as_text=True)   # destaque só uma vez (v0.21.38: depois, no Mostrar)
    assert bcheck(client, k2, CON_B) == "ok"
    assert 'id="blocked-keys"' not in client.get("/admin/").get_data(as_text=True)


def test_tester_blocked_cannot_regen(app, client):
    t, k = _dup_tester(app, client)
    tc = app.test_client()
    login(tc)
    tc.get("/area")
    html = tc.get("/area?lang=pt-BR").get_data(as_text=True)
    text = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", html))
    assert 'id="key-blocked"' in html and ("Sua chave foi bloqueada porque foi usada em outro console. A chave é exclusiva de um console. "
            "Fale com o administrador.") in text
    assert 'action="/area/key"' not in html                            # sem botão de regenerar
    assert "Your key was blocked because it was used on another console." in tc.get("/area?lang=en").get_data(as_text=True)
    assert "Tu clave fue bloqueada porque se usó en otra consola." in tc.get("/area?lang=es").get_data(as_text=True)
    n = db.row("SELECT count(*) AS n FROM beta_keys")["n"]
    r = post(tc, "/area/key", {}, page="/area")
    assert r.status_code == 403 and 'id="betakey"' not in r.get_data(as_text=True)
    assert db.row("SELECT count(*) AS n FROM beta_keys")["n"] == n
    assert bcheck(client, k, CON_A) == "duplicate"
    # depois que o admin desvincula, o testador volta a poder regenerar
    post(client, "/admin/testers/%d/unbind" % t["id"], {}, page="/admin/testers/%d" % t["id"])
    assert post(tc, "/area/key", {}, page="/area").status_code == 200


# ------------------------------------------------------------------ relatos / upload
def _png(w=64, h=48, color=(200, 0, 0)):
    b = io.BytesIO()
    Image.new("RGB", (w, h), color).save(b, "PNG")
    return b.getvalue()


def report(client, files, **over):
    data = {"description": "Tela vermelha ao abrir o jogo", "version": "0.20.1", "console_id": "abcdef0123456789",
            "game": "Fortnite"}
    data.update(over)
    data["photos"] = [(io.BytesIO(b), n) for b, n in files]
    data["csrf"] = csrf(client, "/area")
    return client.post("/area/reports", data=data, content_type="multipart/form-data")


def test_report_with_photo(app, client):
    make_user(app)
    login(client)
    r = report(client, [(_png(), "tela.png")])
    assert r.status_code == 302
    f = db.row("SELECT * FROM report_files")
    st = app.extensions["storage"]
    assert f["r2_key"].startswith("reports/") and st.get(f["r2_key"])[:3] == b"\xff\xd8\xff"   # reencodado JPEG
    assert st.get(f["thumb_key"])
    img = client.get("/media/report/%d" % f["id"])
    assert img.status_code == 200 and img.mimetype == "image/jpeg"
    assert "Tela vermelha" in client.get("/area").get_data(as_text=True)
    # outro testador não vê
    make_user(app, email="o@example.com")
    c2 = app.test_client()
    login(c2, "o@example.com")
    assert c2.get("/media/report/%d" % f["id"]).status_code == 404


def test_upload_rejects_non_images(app, client):
    make_user(app)
    login(client)
    assert report(client, [(b"<?php echo 1; ?>", "x.png")]).status_code == 400
    assert report(client, [(b"GIF89a" + b"\0" * 100, "x.gif")]).status_code == 400
    b = io.BytesIO(); Image.new("RGB", (8, 8)).save(b, "GIF")
    assert report(client, [(b.getvalue(), "real.gif")]).status_code == 400     # GIF de verdade também não
    assert db.row("SELECT count(*) AS n FROM reports")["n"] == 0


def test_upload_limits(app, client):
    make_user(app)
    login(client)
    assert report(client, [(_png(), "a%d.png" % i) for i in range(5)]).status_code == 400   # máx. 4
    app.config["MAX_FILE_BYTES"] = 1000
    try:
        assert report(client, [(_png(400, 400), "big.png")]).status_code == 400
    finally:
        app.config["MAX_FILE_BYTES"] = 8 * 1024 * 1024
    assert report(client, [], description="curto").status_code == 400
    assert report(client, [], console_id="<script>").status_code == 400


def test_admin_reports_status(app, client):
    t = make_user(app)
    tc = app.test_client()
    login(tc)
    report(tc, [(_png(), "a.png")])
    login_admin(client, app)
    html = client.get("/admin/reports?status=new").get_data(as_text=True)
    assert "Tela vermelha" in html
    rid = db.row("SELECT id FROM reports")["id"]
    fid = db.row("SELECT id FROM report_files")["id"]
    assert client.get("/media/report/%d" % fid).status_code == 200          # admin vê foto
    post(client, "/admin/reports/%d" % rid, {"status": "fixed", "note": "corrigido na 0.20.2"}, page="/admin/reports")
    r = db.row("SELECT * FROM reports")
    assert r["status"] == "fixed" and r["admin_note"] == "corrigido na 0.20.2"
    assert "corrigido na 0.20.2" in tc.get("/area").get_data(as_text=True)
    assert client.get("/admin/reports?status=new").get_data(as_text=True).count("data-report-row") == 0
    assert client.get("/admin/reports?status=fixed").get_data(as_text=True).count("data-report-row") == 1
    # linha do tempo: a mudança de status vira evento, visível no detalhe (admin e testador)
    ev = db.row("SELECT * FROM report_events")
    assert ev["status"] == "fixed" and ev["note"] == "corrigido na 0.20.2"
    assert "corrigido na 0.20.2" in client.get("/admin/reports/%d" % rid).get_data(as_text=True)
    assert "corrigido na 0.20.2" in tc.get("/area/reports/%d" % rid).get_data(as_text=True)
    assert client.get("/admin/reports/999999").status_code == 404


# ------------------------------------------------------------------ logs / versões / números
def test_logs_index_and_view(app, client):
    st = app.extensions["storage"]
    doc = {"rx": "2026-10-25T12:00:00+00:00", "ip_h": "x", "console": "abcdef0123456789", "app": "v0.20.1",
           "game": "Fortnite", "tester": "h-t", "lines": ["linha 1", "<b>2</b>"]}
    key = "logs/2026-10-25/abcdef0123456789/120000-deadbeef.json.gz"
    st.put(key, gzip.compress(json.dumps(doc).encode()))
    st.put("logs/2026-10-25/abcdef0123456789/120001-cafebabe.json.gz", b"lixo")
    login_admin(client, app)
    post(client, "/admin/logs/sync", {}, page="/admin/logs")
    assert db.row("SELECT count(*) AS n FROM log_index")["n"] == 2
    html = client.get("/admin/logs?tester=h-t").get_data(as_text=True)
    assert "120000-deadbeef" in html and "120001-cafebabe" not in html
    v = client.get("/admin/logs/view?key=" + key).get_data(as_text=True)
    assert "linha 1" in v and "&lt;b&gt;2&lt;/b&gt;" in v
    assert client.get("/admin/logs/view?key=../etc/passwd").status_code == 404
    o = client.get("/admin/").get_data(as_text=True)
    assert "Fortnite" in o and "v0.20.1" in o


def test_versions_and_promote_request(app, client):
    st = app.extensions["storage"]
    for v, ch in (("0.20.0", "stable"), ("0.21.0-beta.1", "beta")):
        m = {"format": 1, "channel": ch, "version": v, "size": 4, "sha256": "00" * 32,
             "url": "/v1/update/%s/xenon.elf" % v, "notes": "n " + v, "sig": "s"}
        st.put("releases/%s/manifest.json" % v, json.dumps(m).encode())
        st.put("releases/%s/xenon.elf" % v, b"\x7fELF")
        st.put("releases/%s.json" % ch, json.dumps(m).encode())
    login_admin(client, app)
    html = client.get("/admin/versions").get_data(as_text=True)
    assert "0.21.0-beta.1" in html
    post(client, "/admin/versions/promote", {"version": "0.21.0-beta.1"}, page="/admin/versions")
    html = client.get("/admin/versions").get_data(as_text=True)
    assert "release.py promote 0.21.0-beta.1" in html
    assert post(client, "/admin/versions/promote", {"version": "9.9.9"}, page="/admin/versions").status_code == 400
    # testador baixa a beta direto do R2
    make_user(app)
    tc = app.test_client()
    login(tc)
    r = tc.get("/area/download/beta")
    assert r.status_code == 200 and r.data == b"\x7fELF" and "xenon.elf" in r.headers["Content-Disposition"]
    assert app.test_client().get("/area/download/beta").status_code == 302


# ------------------------------------------------------------------ redesign: telas novas
def test_tester_pages_and_isolation(app, client):
    make_user(app)
    login(client)
    assert client.get("/area/report").status_code == 200
    report(client, [(_png(), "a.png")])
    rid = db.row("SELECT id FROM reports")["id"]
    html = client.get("/area/reports").get_data(as_text=True)
    assert "Tela vermelha" in html and "data-report-row" in html
    assert client.get("/area/reports/%d" % rid).status_code == 200
    make_user(app, email="o@example.com")
    c2 = app.test_client()
    login(c2, "o@example.com")
    assert c2.get("/area/reports/%d" % rid).status_code == 404       # relato de outro testador


def test_signup_error_keeps_accept_checked(app, client):
    login_admin(client, app)
    _, link = new_invite(client)
    r = signup(app.test_client(), link, email="nope")
    html = r.get_data(as_text=True)
    assert r.status_code == 400 and 'name="accept" value="1" required checked' in html
    assert 'aria-invalid="true"' in html


def test_admin_pages_render(app, client):
    login_admin(client, app)
    for path in ("/admin/", "/admin/testers?q=adm", "/admin/testers?status=nokey", "/admin/testers?status=blocked", "/admin/invites?state=pending",
                 "/admin/invites?new=1", "/admin/reports?q=x&status=open", "/admin/logs?day=2026-10-25",
                 "/admin/versions", "/admin/features", "/admin/notes", "/admin/notes/new", "/admin/features/new"):
        r = client.get(path)
        assert r.status_code == 200, path
        assert "sidebar" in r.get_data(as_text=True)


def test_dates_are_formatted_by_language(app, client):
    login_admin(client, app)
    db.run("INSERT INTO kv(k, v) VALUES ('logs_synced_at', '2026-10-25T19:35:16+00:00')")
    html = client.get("/admin/?lang=pt-BR").get_data(as_text=True)
    assert "25/10/2026 19:35" in html and "2026-10-25T19:35:16" not in html
    assert "Oct 25, 2026 19:35" in client.get("/admin/?lang=en").get_data(as_text=True)


def test_promote_hides_button_when_pending(app, client):
    st = app.extensions["storage"]
    for v, ch in (("0.21.4", "stable"), ("0.22.0", "beta")):
        m = {"format": 1, "channel": ch, "version": v, "size": 4, "sha256": "00" * 32,
             "url": "/v1/update/%s/xenon.elf" % v, "notes": "n", "sig": "s"}
        st.put("releases/%s/manifest.json" % v, json.dumps(m).encode())
        st.put("releases/%s.json" % ch, json.dumps(m).encode())
    login_admin(client, app)
    assert 'name="version" value="0.22.0"' in client.get("/admin/versions").get_data(as_text=True)
    r = post(client, "/admin/versions/promote", {"version": "0.22.0"}, page="/admin/versions", follow_redirects=True)
    html = r.get_data(as_text=True)
    assert 'name="version" value="0.22.0"' not in html          # sem botão duplicado com pedido pendente
    assert html.count("release.py promote 0.22.0") == 1


def test_status_public_and_admin_crud(app, client):
    from xc360site import seed
    with app.app_context():
        db.run("DELETE FROM kv WHERE k=%s", seed.FLAG)
        assert seed.run() is True and seed.run() is False
    n = db.row("SELECT count(*) AS n FROM features")["n"]
    assert n >= 30
    assert db.row("SELECT published FROM release_notes WHERE version='0.22.0'")["published"] is False
    anon = app.test_client()
    html = anon.get("/status?lang=en").get_data(as_text=True)
    assert "Coming soon" in html and "Wired and wireless Xbox 360 controllers" in html
    assert "Wi-Fi" in anon.get("/status?status=soon").get_data(as_text=True)
    assert anon.get("/status/v/0.22.0").status_code == 404          # rascunho não é público
    home = anon.get("/?lang=es").get_data(as_text=True)
    assert "Próximamente" in home and "Búsqueda de juegos" in home   # home lê o "Em breve" do banco
    login_admin(client, app)
    assert "rascunho" in client.get("/status/v/0.22.0?lang=pt-BR").get_data(as_text=True).lower()
    # cria recurso: inglês/espanhol vazios caem pro português
    r = post(client, "/admin/features/new", {"grp": "video", "status": "beta", "since_version": "0.22.0", "sort": "5",
                                               "name_pt": "Recurso de teste", "desc_pt": "Descrição pt"}, page="/admin/features/new")
    assert r.status_code == 302
    fid = db.row("SELECT id FROM features WHERE name_pt='Recurso de teste'")["id"]
    assert "Recurso de teste" in anon.get("/status?lang=en").get_data(as_text=True)
    assert post(client, "/admin/features/%d" % fid, {"grp": "nope", "status": "beta", "name_pt": ""},
                page="/admin/features/%d" % fid).status_code == 400
    # nota: publica e adiciona item ligado ao recurso
    nid = db.row("SELECT id FROM release_notes WHERE version='0.22.0'")["id"]
    post(client, "/admin/notes/%d" % nid, {"version": "0.22.0", "channel": "beta", "released_on": "2026-10-25",
                                          "summary_pt": "Primeira beta pública", "published": "1"}, page="/admin/notes/%d" % nid)
    post(client, "/admin/notes/%d/items" % nid, {"kind": "known", "text_pt": "Tela vermelha em alguns jogos",
                                                "feature_id": str(fid), "sort": "1"}, page="/admin/notes/%d" % nid)
    html = anon.get("/status/v/0.22.0?lang=pt-BR").get_data(as_text=True)
    assert "Tela vermelha em alguns jogos" in html and "Problema conhecido" in html
    assert "0.22.0" in anon.get("/status").get_data(as_text=True)
    assert post(client, "/admin/notes/new", {"version": "0.22.0", "channel": "beta"}, page="/admin/notes/new").status_code == 400
    iid = db.row("SELECT id FROM release_note_items WHERE kind='known'")["id"]
    post(client, "/admin/notes/%d/items/%d/delete" % (nid, iid), {}, page="/admin/notes/%d" % nid)
    assert not db.row("SELECT 1 FROM release_note_items WHERE id=%s", iid)
    post(client, "/admin/features/%d/delete" % fid, {}, page="/admin/features")
    assert not db.row("SELECT 1 FROM features WHERE id=%s", fid)
    # testador não acessa o CRUD
    make_user(app)
    tc = app.test_client()
    login(tc)
    assert tc.get("/admin/features").status_code == 403


# ------------------------------------------------------------------ filtro por testador (relatos / logs / chave dev)
def _rep(uid, desc, status="new", version="0.22.0"):
    return db.row("INSERT INTO reports(user_id, version, console_id, game, description, status) "
                  "VALUES (%s,%s,'','Halo',%s,%s) RETURNING id", uid, version, desc, status)["id"]


def _log(st, tester, console, n):
    key = "logs/2026-10-25/%s/1200%02d-%08x.json.gz" % (console, n, n)
    st.put(key, gzip.compress(json.dumps({"rx": "2026-10-25T12:00:%02d+00:00" % n, "ip_h": "x", "console": console,
                                          "app": "v0.22.0", "game": "Halo", "tester": tester,
                                          "lines": ["l"]}).encode()))
    return key.rsplit("/", 1)[-1][:15]


def test_admin_reports_filter_by_tester(app, client):
    a = make_user(app, email="ana@example.com", name="Ana Rodrigues")
    b = make_user(app, email="anabela@example.com", name="Ana Rodrigues Filho")   # nome contém o da Ana
    c = make_user(app, email="semnada@example.com", name="Sem Relatos")
    _rep(a["id"], "relato-da-ana-1"); _rep(a["id"], "relato-da-ana-2", status="fixed")
    _rep(b["id"], "relato-do-filho")
    login_admin(client, app)
    for v in (a["id"], a["handle"]):
        h = client.get("/admin/reports?tester=%s" % v).get_data(as_text=True)
        assert h.count("data-report-row") == 2 and "relato-do-filho" not in h, v
        assert "data-tester-chip" in h and 'value="%d" selected' % a["id"] in h
    # status + testador juntos; abas mantêm o testador
    h = client.get("/admin/reports?tester=%d&status=fixed" % a["id"]).get_data(as_text=True)
    assert h.count("data-report-row") == 1 and "relato-da-ana-2" in h
    assert "status=open&amp;tester=%d" % a["id"] in h or "tester=%d&amp;status=open" % a["id"] in h
    # testador sem relatos: vazio, nunca a lista geral
    h = client.get("/admin/reports?tester=%d" % c["id"]).get_data(as_text=True)
    assert h.count("data-report-row") == 0 and "No reports from Sem Relatos" in h
    assert 'value="%d" selected' % c["id"] in h
    # testador inexistente também vem vazio
    for v in ("999999", "nao-existe", "99999999999999"):
        assert client.get("/admin/reports?tester=" + v).get_data(as_text=True).count("data-report-row") == 0, v
    # sem filtro: todos
    assert client.get("/admin/reports").get_data(as_text=True).count("data-report-row") == 3
    # links "relatos deste testador" usam o id (não a busca por nome, que pegava o "Filho" junto)
    link = "/admin/reports?tester=%d" % a["id"]
    assert link in client.get("/admin/testers").get_data(as_text=True)
    assert link in client.get("/admin/testers/%d" % a["id"]).get_data(as_text=True)
    rid = db.row("SELECT id FROM reports WHERE description='relato-da-ana-1'")["id"]
    assert link in client.get("/admin/reports/%d" % rid).get_data(as_text=True)


def test_admin_logs_filter_by_tester(app, client):
    st = app.extensions["storage"]
    a = make_user(app, email="ana@example.com", name="Ana")
    b = make_user(app, email="beto@example.com", name="Beto")
    c = make_user(app, email="semlog@example.com", name="Sem Logs")
    ka = _log(st, a["handle"], "a" * 16, 1)
    kb = _log(st, b["handle"], "b" * 16, 2)
    login_admin(client, app)
    post(client, "/admin/logs/sync", {}, page="/admin/logs")
    for v in (a["handle"], str(a["id"])):
        h = client.get("/admin/logs?tester=" + v).get_data(as_text=True)
        assert ka in h and kb not in h, v
    # testador sem logs: vazio com aviso, e continua selecionado no filtro
    h = client.get("/admin/logs?tester=" + c["handle"]).get_data(as_text=True)
    assert ka not in h and kb not in h and "No logs from Sem Logs" in h
    assert 'value="%s" selected' % c["handle"] in h
    h = client.get("/admin/logs?tester=%d" % c["id"]).get_data(as_text=True)
    assert ka not in h and kb not in h
    # handle desconhecido: vazio
    h = client.get("/admin/logs?tester=fantasma").get_data(as_text=True)
    assert ka not in h and kb not in h
    # link da página do testador
    assert "/admin/logs?tester=" + c["handle"] in client.get("/admin/testers/%d" % c["id"]).get_data(as_text=True)


def test_admin_logs_dev_key_belongs_to_admin(app, client):
    st = app.extensions["storage"]
    a = make_user(app, email="ana@example.com", name="Ana")
    ka = _log(st, a["handle"], "a" * 16, 1)
    kd = _log(st, "dev", "d" * 16, 2)
    adm = login_admin(client, app)
    post(client, "/admin/logs/sync", {}, page="/admin/logs")
    h = client.get("/admin/logs").get_data(as_text=True)
    assert ka in h and kd in h and "Developer (admin)" in h
    # o lote "dev" aparece no filtro do admin dono (por handle, id ou "dev")
    for v in (adm["handle"], str(adm["id"]), "dev"):
        h = client.get("/admin/logs?tester=" + v).get_data(as_text=True)
        assert kd in h and ka not in h, v
    # e não vaza pro filtro de um testador
    h = client.get("/admin/logs?tester=" + a["handle"]).get_data(as_text=True)
    assert ka in h and kd not in h
    # página do admin conta o lote dev e aponta pro filtro dele
    h = client.get("/admin/testers/%d" % adm["id"]).get_data(as_text=True)
    assert "/admin/logs?tester=" + adm["handle"] in h
    with app.app_context():
        from xc360site.views import admin as av
        with app.test_request_context():
            assert av._log_handles(adm) == [adm["handle"], "dev"]
            assert av._log_handles(a) == [a["handle"]]
    v = client.get("/admin/logs/view?key=logs/2026-10-25/%s/%s.json.gz" % ("d" * 16, kd)).get_data(as_text=True)
    assert "Developer (admin)" in v


def test_home_video_file(app, client, monkeypatch):
    app.config["VIDEO_FILE"] = "/v/orbita-beta.mp4"
    app.config["VIDEO_POSTER"] = "/v/orbita-beta-capa.jpg"
    html = client.get("/").get_data(as_text=True)
    assert 'preload="none"' in html and 'src="/v/orbita-beta.mp4"' in html and 'poster="/v/orbita-beta-capa.jpg"' in html


def test_media_env_only_media_paths(monkeypatch):
    from xc360site import config
    monkeypatch.setenv("VIDEO_FILE", "https://evil.example/x.mp4")
    assert config._media("VIDEO_FILE") == ""
    monkeypatch.setenv("VIDEO_FILE", "/v/../x")
    assert config._media("VIDEO_FILE") == ""
    monkeypatch.setenv("VIDEO_FILE", "/v/orbita-beta.mp4")
    assert config._media("VIDEO_FILE") == "/v/orbita-beta.mp4"


def test_media_env_rejects_site_media_prefix(monkeypatch):
    # /media/ é do site (fotos dos relatos); o vídeo fica em /v/
    from xc360site import config
    monkeypatch.setenv("VIDEO_FILE", "/media/orbita-beta.mp4")
    assert config._media("VIDEO_FILE") == ""



# ------------------------------------------------------------------ lista de espera pública
def _wl_join(client, email, reddit):
    return post(client, "/lista", dict(SIGNUP, email=email, reddit=reddit))


def test_waitlist_signup_waits_without_access(app, client):
    r = client.get("/lista")
    assert r.status_code == 200 and 'name="reddit"' in r.get_data(as_text=True)
    r = _wl_join(client, "w1@example.com", "u/Fulano_1")
    assert r.status_code == 302 and r.headers["Location"].endswith("/area")
    u = db.row("SELECT * FROM users WHERE email='w1@example.com'")
    assert u["wait"] == "waiting" and u["reddit"] == "Fulano_1" and u["active"]
    assert 'id="wl-waiting"' in client.get("/area").get_data(as_text=True)
    # sem chave, download nem relatos enquanto espera
    assert client.post("/area/key", data={"csrf": csrf(client, "/area")}).status_code == 302
    assert db.row("SELECT count(*) AS n FROM beta_keys")["n"] == 0
    assert client.get("/area/download/beta").status_code == 302
    assert client.get("/area/report").status_code == 302
    # mesmo usuário do Reddit (com outro formato) não entra de novo
    other = app.test_client()
    assert _wl_join(other, "w2@example.com", "https://www.reddit.com/user/fulano_1/").status_code == 400
    assert _wl_join(other, "w2@example.com", "x").status_code == 400


def test_waitlist_release_draw_and_deadline(app, client):
    from xc360site import waitlist
    for i in range(3):
        assert _wl_join(app.test_client(), "w%d@example.com" % i, "red_%d" % i).status_code == 302
    login_admin(client, app)
    assert "red_0" in client.get("/admin/waitlist").get_data(as_text=True)
    assert "red_0" not in client.get("/admin/testers").get_data(as_text=True)   # a lista não mistura com testadores
    r = post(client, "/admin/waitlist/release", {"n": "2"}, page="/admin/waitlist")
    assert r.status_code == 302
    rel = db.rows("SELECT * FROM users WHERE wait='released' ORDER BY id")
    assert len(rel) == 2 and db.row("SELECT count(*) AS n FROM users WHERE wait='waiting'")["n"] == 1
    # liberado: vê o prazo e gera a chave
    a, b = rel
    ca = app.test_client()
    login(ca, a["email"], SIGNUP["password"])
    html = ca.get("/area").get_data(as_text=True)
    assert 'id="wl-released"' in html
    ka = _newkey(post(ca, "/area/key", {}, page="/area"))
    assert bcheck(client, ka) == "ok"              # usou num console: vira testador normal
    # o outro não usou: 49 h depois sai de vez, com a chave revogada
    cb = app.test_client()
    login(cb, b["email"], SIGNUP["password"])
    kb = _newkey(post(cb, "/area/key", {}, page="/area"))
    db.run("UPDATE users SET released_at = now() - interval '49 hours' WHERE id IN (%s, %s)", a["id"], b["id"])
    assert waitlist.expire() == [b["id"]]
    ua, ub = (db.row("SELECT * FROM users WHERE id=%s", x["id"]) for x in (a, b))
    assert ua["wait"] == "" and ua["active"]
    assert ub["wait"] == "expired" and not ub["active"]
    assert bcheck(client, kb) != "ok"
    assert cb.get("/area").status_code == 302      # sessão encerrada
    assert login(app.test_client(), b["email"], SIGNUP["password"]).status_code != 302   # conta desativada não entra
    assert post(client, "/admin/waitlist/release", {"n": "0"}, page="/admin/waitlist").status_code == 302


def test_waitlist_ip_limit(app):
    from xc360site import waitlist
    c = app.test_client()
    for i in range(waitlist.PER_IP_DAY):
        assert _wl_join(app.test_client(), "ip%d@example.com" % i, "ipuser_%d" % i).status_code == 302
    assert _wl_join(c, "ipx@example.com", "ipuser_x").status_code == 429
