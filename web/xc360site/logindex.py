"""Índice dos lotes de log que a ponte grava no R2.

Objetos: logs/AAAA-MM-DD/<console>/<hhmmss>-<rand>.json.gz com JSON
{rx, ip_h, console, app, game, tester?, lines}. O índice (tabela log_index)
guarda só metadados, pra filtrar por testador/console/dia e tirar números
sem baixar tudo de novo. Sincroniza numa thread (um worker por vez, via
advisory lock): na 1ª vez varre logs/ inteiro; depois só ontem e hoje (UTC).
Junto, as sessões de jogo de cada lote vão pra play_sessions (plays.py); lotes
lidos por um parser mais velho (log_index.play_v < plays.PARSER_V) são relidos
aos poucos (backfill_plays), um lote por vez.
"""
import gzip, io, json, re, threading, time, zlib
from datetime import datetime, timedelta, timezone

from . import db, plays

KEY_RE = re.compile(r"^logs/(\d{4}-\d{2}-\d{2})/([0-9a-f]{16,64})/(\d{6})-[0-9a-f]{1,32}\.json\.gz$")
PLAY_RE = re.compile(r"\[xn\] PLAY \S+ \((.+?)\)")
RX_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}([+-]\d{2}:\d{2}|Z)?$")
MAX_RAW = 32 * 1024 * 1024
LOCK_ID = 36037


def read_batch(storage, key):
    if not KEY_RE.match(key or ""):
        return None
    raw = storage.get(key)
    if raw is None:
        return None
    with gzip.GzipFile(fileobj=io.BytesIO(raw)) as g:
        data = g.read(MAX_RAW + 1)
    if len(data) > MAX_RAW:
        raise ValueError("lote grande demais")
    doc = json.loads(data)
    return doc, len(raw)


def _rx(v):
    return v if isinstance(v, str) and RX_RE.match(v) else None


def _s(v, n):
    return str(v if v is not None else "").replace("\x00", "")[:n]


def sync(storage, max_fetch=300):
    """Indexa lotes novos. Devolve quantos indexou (ou -1 se outro worker está sincronizando)."""
    with db.conn() as c:
        if not c.execute("SELECT pg_try_advisory_xact_lock(%s) AS ok", (LOCK_ID,)).fetchone()["ok"]:
            return -1
        try:
            backfilled = c.execute("SELECT v FROM kv WHERE k='logs_backfill'").fetchone()
            if backfilled:
                today = datetime.now(timezone.utc).date()
                keys = []
                for d in (today - timedelta(days=1), today):
                    keys += storage.list("logs/%s/" % d.isoformat())[0]
            else:
                keys = storage.list("logs/")[0]
            keys = [k for k in keys if KEY_RE.match(k)]
            have = {r["r2_key"] for r in c.execute(
                "SELECT r2_key FROM log_index WHERE r2_key = ANY(%s)", (keys,)).fetchall()}
            todo = [k for k in keys if k not in have]
            n = 0
            psalt = plays.salt(c)
            for k in todo[:max_fetch]:
                day, console, _ = KEY_RE.match(k).groups()
                doc = None
                try:
                    doc, size = read_batch(storage, k)
                    lines = doc.get("lines") or []
                    game = doc.get("game") or ""
                    if not game:
                        # app <= 0.20.4 mandava o lote do jogo com "game" vazio:
                        # tira o nome da linha "[xn] PLAY <ID> (<nome>)"
                        for ln in lines:
                            m = PLAY_RE.search(str(ln))
                            if m:
                                game = m.group(1)
                                break
                    rec = (k, day, console, _s(doc.get("tester"), 64), _s(doc.get("app"), 32),
                           _s(game, 80), _rx(doc.get("rx")), len(lines), size)
                except Exception:
                    doc, rec = None, (k, day, console, "", "?", "?", None, 0, 0)
                c.execute("INSERT INTO log_index(r2_key, day, console, tester, app, game, rx, nlines, size) "
                          "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s) ON CONFLICT DO NOTHING", rec)
                _store_plays(c, k, day, rec[6], doc, psalt)
                n += 1
            backfill_plays(c, storage, max(0, max_fetch - n), psalt)
            if not backfilled and len(todo) <= max_fetch:
                c.execute("INSERT INTO kv(k, v) VALUES ('logs_backfill', '1') ON CONFLICT DO NOTHING")
            c.execute("INSERT INTO kv(k, v) VALUES ('logs_synced_at', %s) ON CONFLICT (k) DO UPDATE SET v=excluded.v",
                      (datetime.now(timezone.utc).isoformat(timespec="seconds"),))
            c.execute("DELETE FROM login_attempts WHERE at < now() - interval '2 days'")
            c.execute("DELETE FROM sessions WHERE expires_at < now()")
            return n
        finally:
            pass   # trava de transação: solta no fim do bloco (antes ficava presa se algo abortasse)


def _store_plays(c, key, day, rx, doc, psalt):
    try:
        with c.transaction():        # savepoint: um lote ruim não derruba o sync
            plays.store(c, key, day, rx, doc if isinstance(doc, dict) else {}, psalt)
    except Exception:
        c.execute("UPDATE log_index SET play_v=%s WHERE r2_key=%s", (plays.PARSER_V, key))


def backfill_plays(c, storage, limit, psalt=None):
    """Relê do R2 os lotes ainda não processados pelo parser atual. Devolve quantos leu."""
    if limit <= 0:
        return 0
    psalt = psalt or plays.salt(c)
    todo = c.execute("SELECT r2_key, day, rx FROM log_index WHERE play_v < %s ORDER BY day DESC, r2_key DESC LIMIT %s",
                     (plays.PARSER_V, limit)).fetchall()
    done = 0
    for r in todo:
        try:
            got = read_batch(storage, r["r2_key"])
        except (gzip.BadGzipFile, EOFError, zlib.error, ValueError):
            got = None               # lote corrompido: marca como lido (sem sessões)
        except OSError:
            continue                 # R2 fora: tenta de novo no próximo ciclo
        except Exception:
            got = None               # lote corrompido: marca como lido (sem sessões)
        _store_plays(c, r["r2_key"], r["day"], r["rx"], got[0] if got else None, psalt)
        done += 1
    return done


def backfill_all(storage, chunk=200):
    """Backfill único (CLI): processa todos os lotes pendentes, em blocos."""
    total = 0
    while True:
        with db.conn() as c:
            if not c.execute("SELECT pg_try_advisory_xact_lock(%s) AS ok", (LOCK_ID,)).fetchone()["ok"]:
                return -1
            try:
                left = c.execute("SELECT count(*) AS n FROM log_index WHERE play_v < %s",
                                 (plays.PARSER_V,)).fetchone()["n"]
                if not left:
                    return total
                n = backfill_plays(c, storage, chunk)
                total += n
            finally:
                pass   # trava de transação: solta no fim do bloco (antes ficava presa se algo abortasse)
        if n == 0:
            return total


def start_thread(app):
    interval = app.config["LOG_SYNC_INTERVAL"]
    if interval <= 0:
        return

    def loop():
        time.sleep(10)
        while True:
            with app.app_context():
                try:
                    sync(app.extensions["storage"])
                except Exception as e:
                    app.logger.warning("sync de logs falhou: %s", e)
                try:
                    from . import waitlist
                    waitlist.expire()
                except Exception as e:
                    app.logger.warning("expiração da lista de espera falhou: %s", e)
            time.sleep(interval)
    threading.Thread(target=loop, name="logsync", daemon=True).start()
