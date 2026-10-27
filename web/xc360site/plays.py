"""Sessões de jogo tiradas dos lotes de log (página pública /jogos).

Cada lote ({rx, ip_h, console, app, game, tester?, lines}) vira 0..N "trechos":
um por linha "[xn] PLAY <TITLEID> (<Nome>): sessao no Xbox" e, se o lote começa
no meio de um jogo (sessão longa partida em vários lotes), um trecho de
continuação com o que vem antes do 1º PLAY. Por trecho guardamos FPS (linhas
STAT a cada 2 s, sem os primeiros ~4 s depois de abrir/reconectar), quadros
pulados, kbps, versão do app e como terminou (ok / queda / erro / travou).

Privacidade: o console e o testador entram só como hash com sal aleatório do
banco (kv.play_salt); nada disso sai na página pública, só contagens.
"""
import hashlib, json, re, secrets, statistics
from datetime import timedelta

PARSER_V = 2                   # sobe quando o parser muda -> o backfill reprocessa os lotes
WARMUP_S = 4                   # ignora STAT dos primeiros ~4 s de cada sessão/reconexão
FPS_OK = 55                    # "% do tempo >= 55 fps"

PLAY_RE = re.compile(r"\[xn\] PLAY (\S+) \((.+?)\)[^:]*: sessao")
STREAM_RE = re.compile(r"\[sess\] STATE STREAMING (.+?)\s*$")
STAT_RE = re.compile(r"^\s*STAT t=")
KV_RE = re.compile(r"\b([a-z_]+)=(-?\d+(?:\.\d+)?)\b")
RECONN_RE = re.compile(r"\[sess\] STATE RECONNECTING")
DROP_RE = re.compile(r"\[menu\] v[ií]deo perdido|\[sess\] queda no meio do jogo")
ERROR_RE = re.compile(r"\[sess\] STATE ERROR\b(.*)")
NOT_GAME_ERR_RE = re.compile(r"login", re.I)       # "Faca o login no Xbox primeiro": não é problema do jogo
HANG_RE = re.compile(r"\[wdog\] (?:TRAVOU|CRASH)\b|Segmentation fault|\bkernel panic\b")
TITLE_RE = re.compile(r"^[A-Z0-9_]{2,64}$")
# v2: motivo de fim que o servidor manda ({"reason":"KickFor..."} no canal de mensagens)
KICK_RE = re.compile(r'reason\\*"\s*:\s*\\*"(Kick\w+)')
OWNED_RE = re.compile(r"SessionOwnedByAnotherInstance")
# fim normal da sessão: não é queda do app (tempo do gratuito, saiu pelo menu,
# a conta abriu em outro aparelho, inatividade)
BENIGN_KICKS = {"KickForOutOfStreamingTime", "KickForStopCommand", "KickByNewSession", "KickForBeingIdle"}
CLOSED_KICK = "KickForClosedGame"   # o jogo fechou sozinho no servidor (não abre no app)
OUTCOMES = ("ok", "drop", "error", "hang", "nostream")


def _num(kv, k):
    v = kv.get(k)
    try:
        return float(v) if v is not None else None
    except ValueError:
        return None


class _Seg:
    def __init__(self, title_id="", name="", continued=False):
        self.title_id, self.name, self.continued = title_id, name, continued
        self.stream_name = ""
        self.fps, self.kbps, self.loss = [], [], []
        self.skipped = 0
        self._last_skipped = None
        self.drops = self.errors = self.hangs = 0
        self.kick = ""
        self._pre = (0, 0)              # quedas/erros antes do motivo do servidor
        self.warm = not continued       # espera a 1ª STAT pra marcar o início
        self.t0 = None
        self.warm_n = 0

    def stat(self, line):
        kv = dict(KV_RE.findall(line))
        t, fps = _num(kv, "t"), _num(kv, "dec_fps")
        sk = _num(kv, "skipped")
        if sk is not None:              # contador cumulativo; queda = reinício do contador
            if self._last_skipped is not None:
                self.skipped += int(sk - self._last_skipped) if sk >= self._last_skipped else int(sk)
            self._last_skipped = sk
        if self.warm:
            if self.t0 is None:
                self.t0 = t
            self.warm_n += 1
            if (t is not None and self.t0 is not None and t - self.t0 < WARMUP_S) or \
               (t is None and self.warm_n <= WARMUP_S // 2):
                return
            self.warm = False
        kbps = _num(kv, "kbps")
        if fps is None or (fps < 1 and not kbps):     # menu/pausa: não é jogo rodando
            return
        self.fps.append(fps)
        if kbps:
            self.kbps.append(kbps)
        loss = _num(kv, "loss_pct")
        if loss is not None:
            self.loss.append(loss)

    def restart_warmup(self):
        self.warm, self.t0, self.warm_n = True, None, 0

    def settle(self):
        """O que veio depois de um fim normal (ou do jogo fechar) é consequência, não queda."""
        if self.kick in BENIGN_KICKS:
            self.drops, self.errors = self._pre
        elif self.kick == CLOSED_KICK:
            self.drops = self.errors = 0

    def outcome(self):
        if self.hangs:
            return "hang"
        if self.errors:
            return "error"
        if self.drops:
            return "drop"
        return "ok" if self.fps else "nostream"

    def useful(self):
        return bool(self.title_id or self.fps or self.drops or self.errors or self.hangs or self.kick)

    def result(self, fallback_name):
        name = (self.name or self.stream_name or fallback_name or "").strip()[:80]
        fps = self.fps
        hist = {}
        for v in fps:
            b = str(max(0, min(70, int(round(v)))))
            hist[b] = hist.get(b, 0) + 1
        r = lambda v, n=1: round(v, n) if v is not None else None
        self.settle()
        return {
            "title_id": self.title_id, "game": name, "continued": self.continued, "kick": self.kick[:40],
            "outcome": self.outcome(), "n_drops": self.drops, "n_errors": self.errors, "n_hangs": self.hangs,
            "samples": len(fps), "ge55": sum(1 for v in fps if v >= FPS_OK),
            "fps_avg": r(statistics.fmean(fps)) if fps else None,
            "fps_min": r(min(fps)) if fps else None,
            "fps_med": r(statistics.median(fps)) if fps else None,
            "hist": json.dumps(hist, separators=(",", ":")),
            "skipped": self.skipped,
            "kbps_avg": r(statistics.fmean(self.kbps), 0) if self.kbps else None,
            "loss_avg": r(statistics.fmean(self.loss), 2) if self.loss else None,
        }


def parse(lines, doc_game=""):
    """Linhas de um lote -> lista de trechos (dicts). Só os que têm jogo e algo a dizer."""
    segs = [_Seg(continued=True)]
    for raw in lines or []:
        ln = str(raw)
        cur = segs[-1]
        if STAT_RE.match(ln):
            cur.stat(ln)
            continue
        m = PLAY_RE.search(ln)
        if m:
            tid = m.group(1) if TITLE_RE.match(m.group(1)) else ""
            segs.append(_Seg(tid, m.group(2)))
            continue
        m = STREAM_RE.search(ln)
        if m:
            if not cur.stream_name:
                cur.stream_name = m.group(1)[:80]
            continue
        if RECONN_RE.search(ln):
            cur.restart_warmup()
            continue
        m = KICK_RE.search(ln)
        if m or OWNED_RE.search(ln):
            if not cur.kick:
                cur.kick = m.group(1) if m else "KickByNewSession"
                cur._pre = (cur.drops, cur.errors)
            continue
        if DROP_RE.search(ln):
            cur.drops += 1
            continue
        m = ERROR_RE.search(ln)
        if m:
            if not NOT_GAME_ERR_RE.search(m.group(1)):
                cur.errors += 1
            continue
        if HANG_RE.search(ln):
            cur.hangs += 1
    out = []
    for s in segs:
        if not s.useful():
            continue
        res = s.result(doc_game if s.continued else "")
        if res["game"]:
            out.append(res)
    return out


# ------------------------------------------------------------------ gravação
def salt(c):
    r = c.execute("SELECT v FROM kv WHERE k='play_salt'").fetchone()
    if r:
        return r["v"]
    c.execute("INSERT INTO kv(k, v) VALUES ('play_salt', %s) ON CONFLICT DO NOTHING", (secrets.token_hex(16),))
    return c.execute("SELECT v FROM kv WHERE k='play_salt'").fetchone()["v"]


def anon(s, value):
    return hashlib.sha256((s + "|" + (value or "")).encode()).hexdigest()[:16] if value else ""


COLS = ("title_id", "kick", "game", "continued", "outcome", "n_drops", "n_errors", "n_hangs", "samples", "ge55",
        "fps_avg", "fps_min", "fps_med", "hist", "skipped", "kbps_avg", "loss_avg")


def store(c, key, day, rx, doc, s=None):
    """Grava os trechos de um lote (substitui os anteriores) e marca o lote como processado."""
    s = s or salt(c)
    c.execute("DELETE FROM play_sessions WHERE r2_key=%s", (key,))
    app = str(doc.get("app") or "")[:32] if isinstance(doc, dict) else ""
    if isinstance(doc, dict) and app not in ("", "?", "site-smoke"):
        console = str(doc.get("console") or "")[:64]
        tester = str(doc.get("tester") or "")[:64]
        con_h = anon(s, "c:" + console) if console else ""
        who_h = anon(s, "t:" + tester) if tester else con_h
        for i, p in enumerate(parse(doc.get("lines"), str(doc.get("game") or ""))):
            c.execute("INSERT INTO play_sessions(r2_key, seq, day, rx, app, console_h, tester_h, " + ", ".join(COLS) +
                      ") VALUES (%s,%s,%s,%s,%s,%s,%s," + ",".join(["%s"] * len(COLS)) + ")",
                      (key, i, day, rx, app, con_h, who_h) + tuple(p[k] for k in COLS))
    c.execute("UPDATE log_index SET play_v=%s WHERE r2_key=%s", (PARSER_V, key))


# ------------------------------------------------------------------ agregação (página pública)
def norm_name(n):
    return re.sub(r"\s+", " ", (n or "").strip()).lower()


def _hist_median(h):
    n = sum(h.values())
    if not n:
        return None
    acc = 0
    for b in sorted(h):
        acc += h[b]
        if acc * 2 >= n:
            return b
    return None


def _ver(v):
    return (v or "").lstrip("v")


def _vkey(v):
    return tuple(int(x) if x.isdigit() else -1 for x in re.split(r"[.+-]", _ver(v)))


BADGES = ("good", "unstable", "bad", "noopen")
SAME_SESSION = timedelta(hours=6)    # lote de continuação até 6 h depois de um PLAY do mesmo testador = mesma sessão


def target_fps(med):
    """60 ou 30: o xCloud transmite alguns jogos a 30 fps; esses são medidos contra 30."""
    if med is None:
        return None
    return 30 if 27 <= med <= 32 else 60


def pct_at_target(hist, target):
    n = sum(hist.values())
    if not n or not target:
        return None
    cut = FPS_OK if target == 60 else 28
    return round(100.0 * sum(v for b, v in hist.items() if b >= cut) / n)


def badge(g):
    """good / unstable / bad a partir de limites simples (ver gm.d_* nas traduções)."""
    s = max(1, g["sessions"])
    bad_rate = (g["errors"] + g["hangs"]) / s
    drop_rate = g["drops"] / s
    med, pct = g["fps_med"], g["pct_ok"]
    if med is None:
        return "bad" if (g["errors"] or g["hangs"] or g["drops"]) else "unstable"
    if (g["target"] == 60 and med < 45) or pct < 40 or bad_rate >= 0.5:
        return "bad"
    if pct >= 90 and bad_rate <= 0.2 and drop_rate <= 0.25:
        return "good"
    return "unstable"


def _session_rows(rs):
    """Trechos que contam como sessão: os que começam com PLAY e as continuações sem PLAY conhecido antes
    (o lote do PLAY nunca chegou ou é de outro testador)."""
    out = [r for r in rs if not r["continued"]]
    seen = {}
    for r in out:
        seen.setdefault(r["tester_h"], []).append(r["rx"])
    for r in sorted((r for r in rs if r["continued"]), key=lambda r: (r["rx"] is None, r["rx"] or 0)):
        prev = seen.get(r["tester_h"], [])
        if not any(p is None or r["rx"] is None or timedelta(0) <= r["rx"] - p <= SAME_SESSION for p in prev):
            out.append(r)
        seen.setdefault(r["tester_h"], []).append(r["rx"])
    return out


def games(rows, hidden=(), current=None):
    """Linhas de play_sessions -> (lista por jogo, resumo). `hidden`: chaves de jogo escondidas pelo admin.
    `current`: versão beta atual. Erros/travamentos de versões anteriores a ela contam como corrigidos (não
    aparecem nem pesam no selo); quedas só aparecem enquanto não houver mais sessões boas do que quedas."""
    cur_k = _vkey(current) if current else None
    name2tid = {}
    for r in rows:
        if r["title_id"]:
            name2tid.setdefault(norm_name(r["game"]), r["title_id"])
    groups = {}
    for r in rows:
        if r["outcome"] == "nostream" and r.get("kick") != CLOSED_KICK:
            continue
        gk = r["title_id"] or name2tid.get(norm_name(r["game"])) or norm_name(r["game"])
        if not gk:
            continue
        groups.setdefault(gk, []).append(r)
    out, all_who, ver_count = [], set(), {}
    for gk, rs in groups.items():
        names = {}
        for r in ([r for r in rs if r["title_id"]] or rs):
            names[r["game"]] = names.get(r["game"], 0) + 1
        hist = {}
        samples = ge55 = 0
        for r in rs:
            samples += r["samples"] or 0
            ge55 += r["ge55"] or 0
            try:
                for b, n in json.loads(r["hist"] or "{}").items():
                    hist[int(b)] = hist.get(int(b), 0) + int(n)
            except (ValueError, TypeError, AttributeError):
                pass
        sess = _session_rows(rs)
        last = max(rs, key=lambda r: (r["rx"] is not None, r["rx"] or 0, r["r2_key"]))
        med = _hist_median(hist)
        g = {
            "key": gk, "title_id": gk if TITLE_RE.match(gk) else "",
            "name": max(names, key=lambda n: (names[n], len(n))),
            "sessions": max(1, len(sess)),
            "testers": len({r["tester_h"] for r in rs if r["tester_h"]}),
            "fps_med": med, "pct55": round(100.0 * ge55 / samples) if samples else None,
            "target": target_fps(med), "samples": samples, "minutes": round(samples * 2 / 60.0),
            # a queda que veio junto com um erro de versão anterior é o mesmo problema (corrigido)
            "drops": sum(1 for r in rs if r["n_drops"] and not (r["n_errors"] and cur_k is not None
                                                                and _vkey(r["app"]) < cur_k)),
            "errors": sum(1 for r in rs if r["n_errors"] and (cur_k is None or _vkey(r["app"]) >= cur_k)),
            "hangs": sum(1 for r in rs if r["n_hangs"] and (cur_k is None or _vkey(r["app"]) >= cur_k)),
            "ok_sessions": sum(1 for r in sess if r["outcome"] == "ok" and r.get("kick") != CLOSED_KICK),
            "closed": sum(1 for r in rs if r.get("kick") == CLOSED_KICK),
            "last_version": _ver(last["app"]), "last_at": last["rx"] or last["day"],
        }
        if g["ok_sessions"] > g["drops"]:   # mais sessões boas do que quedas: a queda sai da página
            g["drops"] = 0
        g["pct_ok"] = pct_at_target(hist, g["target"])
        g["old"] = bool(cur_k and g["last_version"] and _vkey(g["last_version"]) < cur_k)
        g["status"] = badge(g)
        # fechou no servidor em toda tentativa e nunca rodou de verdade: "não abre"
        if g["closed"] and not g["ok_sessions"] and (g["samples"] or 0) < 30:
            g["status"] = "noopen"
        g["few"] = g["sessions"] < 3
        g["hidden"] = gk in hidden
        out.append(g)
        if not g["hidden"]:
            all_who |= {r["tester_h"] for r in rs if r["tester_h"]}
            for r in (sess or rs[:1]):
                v = _ver(r["app"])
                if v:
                    ver_count[v] = ver_count.get(v, 0) + 1
    out.sort(key=lambda g: (-g["sessions"], -g["samples"], g["name"].lower()))
    vis = [g for g in out if not g["hidden"]]
    top_ver = max(ver_count, key=lambda v: (ver_count[v], _vkey(v))) if ver_count else None
    summary = {"games": len(vis), "sessions": sum(g["sessions"] for g in vis), "testers": len(all_who),
               "top_version": top_ver, "top_version_n": ver_count.get(top_ver, 0) if top_ver else 0,
               "minutes": sum(g["minutes"] for g in vis)}
    return out, summary


# ------------------------------------------------------------------ relatos em forma pública
EMAIL_RE = re.compile(r"[\w.+-]+@[\w-]+(\.[\w-]+)+")
PHONE_RE = re.compile(r"\+?\(?\d[\d\s()-]{7,}\d")
VER_OK = re.compile(r"^v?\d+\.\d+(\.\d+)?([.+-][0-9A-Za-z.]{1,20})?$")


def scrub(text, n=600):
    t = EMAIL_RE.sub("[…]", text or "")
    t = PHONE_RE.sub(lambda m: "[…]" if sum(ch.isdigit() for ch in m.group(0)) >= 10 else m.group(0), t)
    t = t.strip()
    return t if len(t) <= n else t[:n].rsplit(" ", 1)[0] + "…"


def public_report(r, known_games):
    """Só o que é seguro: jogo (se for um jogo conhecido), versão, status, datas e a nossa resposta."""
    g = known_games.get(norm_name(r["game"]))
    return {"id": r["id"], "game": g, "version": _ver(r["version"]) if VER_OK.match(r["version"] or "") else "",
            "status": r["status"], "note": scrub(r["admin_note"]), "created_at": r["created_at"],
            "updated_at": r["updated_at"]}
