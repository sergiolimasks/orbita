#!/usr/bin/env python3
"""XCloud360 launcher -- PC-side daemon the Xbox talks to to pick a game.

The Xbox (app/h264play) opens a TCP control connection (default :9002) and
speaks a line protocol (UTF-8, '\\n'-terminated, TAB-separated fields):

  Xbox -> PC : HELLO<TAB>ver | LIST | PLAY<TAB>TITLEID | STOP | PAUSE | PING
  PC -> Xbox : STATE<TAB>CODE<TAB>text | LOGIN<TAB>url<TAB>code
               GAME<TAB>TITLEID<TAB>name<TAB>recent(0|1) ... END | PONG

CODE: IDLE LOGIN CATALOG STARTING QUEUE STREAMING RECONNECTING ERROR.

The launcher runs tools/xcloud-capture/rtp/capture-rtp for the chosen title
(video goes Xbox <- capture-rtp on --video-port, unchanged), follows its log
to report queue/provisioning/streaming, restarts it when the xCloud session
dies (process exit or video AU counter frozen) and drives `greennx login`
when the saved Microsoft login is missing/expired.

v0.19.3: PAUSE = the Xbox went back to its menu (Back+Start) WITHOUT ending
the game: capture-rtp keeps running (keepalive, video to :9000 that the
Xbox drops). PLAY of the same title resumes it (just STATE again; the new
:9000 connection starts at an IDR), PLAY of another title replaces it as
before, STOP ends it. While paused nothing is restarted: if the session
dies the launcher goes to IDLE. Older Xboxes never send PAUSE (STOP as
before); older launchers ignore it ("unknown command").

Python stdlib only. See README.md.
"""

import argparse
import collections
import itertools
import json
import os
import pathlib
import queue
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import traceback
import unicodedata

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_REPO = HERE.parent.parent
HOME = pathlib.Path(os.path.expanduser("~"))

STATE_CODES = ("IDLE", "LOGIN", "CATALOG", "STARTING", "QUEUE", "STREAMING",
               "RECONNECTING", "ERROR")

# --------------------------------------------------------------------------
# logging
# --------------------------------------------------------------------------
_log_lock = threading.Lock()


def log(msg):
    t = time.time()
    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t))
    with _log_lock:
        sys.stdout.write("%s.%03d %s\n" % (ts, int((t % 1) * 1000), msg))
        sys.stdout.flush()


# --------------------------------------------------------------------------
# text helpers
# --------------------------------------------------------------------------
def ascii_fold(s, maxlen=60):
    """NFKD-fold to plain ASCII; drop TM/R/C signs and control chars."""
    s = re.sub("[\u2122\u00ae\u00a9\u2120]", "", s or "")
    s = unicodedata.normalize("NFKD", s)
    s = s.encode("ascii", "ignore").decode("ascii")
    s = re.sub(r"[\x00-\x1f\x7f]+", " ", s)
    s = re.sub(r" {2,}", " ", s).strip()
    if maxlen and len(s) > maxlen:
        s = s[:maxlen - 3].rstrip() + "..."
    return s


def proto_field(s):
    return re.sub(r"[\t\r\n]+", " ", str(s))


TITLE_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.\-]{0,79}$")
GAME_LINE_RE = re.compile(r"^  ([A-Z0-9][A-Z0-9_]*)(?:\s{2,}(\S.*?))?\s*$")
DEVICE_CODE_RE = re.compile(
    r"To sign in,\s+open\s+(\S+)\s+and\s+enter\s+code:\s+([A-Za-z0-9-]+)",
    re.IGNORECASE | re.DOTALL)
# How green-nx reports a missing/expired login (core/auth.cpp, cli/main.cpp,
# capture-rtp main()).
AUTH_RE = re.compile(r"not signed in|no saved login|login expired|"
                     r"sign in again|invalid_grant", re.IGNORECASE)
AU_RE = re.compile(r"video:\s*(\d+)\s+AUs")
CAP_ERROR_RES = [
    (re.compile(r"session failed: ?(.*)"), "Sessao xCloud falhou"),
    (re.compile(r"start_cloud failed: ?(.*)"), "Falha ao iniciar jogo no xCloud"),
    (re.compile(r"all \d+ attempts failed"), "xCloud nao conectou (midia morta)"),
    (re.compile(r"TCP-out failed to bind"), "Porta de video ocupada"),
    (re.compile(r"what\(\):\s*(.*)"), "Erro no capture-rtp"),
    (re.compile(r"exchange_sdp failed: ?(.*)"), "Falha na negociacao SDP"),
]


def queue_info(obj):
    """Pull (position, eta_seconds) out of a /state JSON, wherever it is."""
    pos, eta, eta_any = None, None, None

    def walk(o):
        nonlocal pos, eta, eta_any
        if isinstance(o, dict):
            for k, v in o.items():
                kl = k.lower()
                if isinstance(v, (int, float)) and not isinstance(v, bool):
                    if pos is None and ("position" in kl or "placeinqueue" in kl):
                        pos = int(v)
                    elif kl == "estimatedtotalwaittimeinseconds":
                        eta = int(v)
                    elif eta_any is None and "wait" in kl and "second" in kl:
                        eta_any = int(v)
                else:
                    walk(v)
        elif isinstance(o, list):
            for x in o:
                walk(x)

    walk(obj)
    return pos, (eta if eta is not None else eta_any)


def queue_text(pos, eta):
    parts = []
    if pos is not None:
        parts.append("posicao %d" % pos)
    if eta is not None:
        parts.append("<1 min" if eta < 60 else "~%d min" % ((eta + 59) // 60))
    return ("Na fila: " + ", ".join(parts)) if parts else "Na fila do xCloud..."


def port_free(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def procs_named(name):
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % d) as f:
                if f.read().strip() == name:
                    out.append(int(d))
        except OSError:
            pass
    return out


def write_json_atomic(path, data):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)


def read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


# --------------------------------------------------------------------------
# Xbox control connection
# --------------------------------------------------------------------------
class Client:
    _ids = itertools.count(1)

    def __init__(self, sock, addr):
        self.id = next(Client._ids)
        self.sock = sock
        self.addr = "%s:%d" % addr
        self.lock = threading.Lock()
        self.alive = True

    def send_lines(self, rows):
        data = "".join("\t".join(proto_field(f) for f in r) + "\n" for r in rows)
        try:
            with self.lock:
                self.sock.sendall(data.encode("utf-8"))
            for r in rows[:3]:
                log("-> #%d %s" % (self.id, "\t".join(map(str, r)).replace("\t", " | ")))
            if len(rows) > 3:
                log("-> #%d ... (%d more lines)" % (self.id, len(rows) - 3))
            return True
        except OSError as e:
            log("[client #%d] send failed: %s" % (self.id, e))
            self.close()
            return False

    def send(self, *fields):
        return self.send_lines([fields])

    def close(self):
        if self.alive:
            self.alive = False
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass


class CaptureRun:
    def __init__(self, gen, title, proc, log_path):
        now = time.monotonic()
        self.gen, self.title, self.proc, self.log_path = gen, title, proc, log_path
        self.started = now
        self.last_line_t = now
        self.au = 0
        self.au_t = now
        self.connected = False      # "[cap] CONNECTED" seen
        self.streaming = False      # AU counter grew after CONNECTED
        self.creds_ok = False
        self.auth_fail = False
        self.last_error = None
        self.sess_state = None
        self.stopping = False
        self.sctp_errs = 0
        self.sctp_logged = 0
        self.sctp_log_t = 0.0
        self.last_stat_log = 0.0
        self.last_xbox_log = 0.0


# --------------------------------------------------------------------------
# the daemon
# --------------------------------------------------------------------------
class Launcher:
    def __init__(self, a):
        self.a = a
        self.ev = queue.Queue()
        self.client = None
        self.state = ("IDLE", "Pronto")
        self.state_before_catalog = None
        # catalog
        self.catalog = read_json(a.catalog_cache, None)
        self.catalog_fetching = False
        self.catalog_failed = False
        self.list_wanted = False
        # recents
        st = read_json(a.state_file, {})
        self.recents = [t for t in st.get("recent", []) if isinstance(t, str)][:10]
        # login
        self.login_proc = None
        self.login_info = None
        self.after_login = None
        self.last_login_ok = 0.0
        self.play_after_login = None   # title resumed right after a login
        # capture
        self.title = None           # what the user wants running (None = idle)
        self.cap = None             # current CaptureRun
        self.gen = 0
        self.killing = 0            # captures being stopped right now
        self.start_pending = False
        self.restart_at = None
        self.reconnecting = False
        self.restarts = collections.deque()
        self.paused = False         # v0.19.3: Xbox in its menu, game kept open (PAUSE)
        self.disconnected_at = None
        self.shutting_down = False

    # ---------------- state ----------------
    def set_state(self, code, text, force=False):
        assert code in STATE_CODES
        text = ascii_fold(text, 80)
        if (code, text) == self.state and not force:
            return
        self.state = (code, text)
        log("STATE %s  %s" % (code, text))
        if self.client:
            self.client.send("STATE", code, text)

    def game_name(self, title):
        for g in (self.catalog or {}).get("games", []):
            if g["id"] == title:
                return g["name"] or title
        return title

    # ---------------- main loop ----------------
    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.a.bind, self.a.port))
        srv.listen(4)
        log("launcher listening on %s:%d  (video port %d, capture %s)"
            % (self.a.bind, self.a.port, self.a.video_port, self.a.capture_bin))
        log("catalog cache %s (%s), state %s, recents=%s"
            % (self.a.catalog_cache,
               "%d games" % len(self.catalog["games"]) if self.catalog else "empty",
               self.a.state_file, self.recents))
        threading.Thread(target=self._accept_loop, args=(srv,), daemon=True).start()
        threading.Thread(target=self._ticker, daemon=True).start()
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda *_: self.ev.put(("shutdown",)))
        while True:
            ev = self.ev.get()
            try:
                if ev[0] == "shutdown":
                    self._shutdown()
                    return
                getattr(self, "_on_" + ev[0])(*ev[1:])
            except Exception:
                log("INTERNAL ERROR handling %s:\n%s" % (ev[0], traceback.format_exc()))

    def _accept_loop(self, srv):
        while True:
            try:
                sock, addr = srv.accept()
            except OSError as e:
                log("accept failed: %s" % e)
                time.sleep(1)
                continue
            self.ev.put(("client", sock, addr))

    def _ticker(self):
        while True:
            time.sleep(0.5)
            self.ev.put(("tick",))

    def _reader(self, c):
        buf = b""
        try:
            while True:
                try:
                    data = c.sock.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.ev.put(("cmd", c.id, line.decode("utf-8", "replace").rstrip("\r")))
                if len(buf) > 8192:
                    buf = b""
        except OSError:
            pass
        self.ev.put(("client_closed", c.id))

    # ---------------- client events ----------------
    def _on_client(self, sock, addr):
        sock.settimeout(10)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for opt, val in (("TCP_KEEPIDLE", 10), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
            if hasattr(socket, opt):
                sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), val)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c = Client(sock, addr)
        if self.client:
            log("[client #%d] replaced by new connection #%d" % (self.client.id, c.id))
            self.client.close()
        self.client = c
        if self.disconnected_at is not None:
            log("[client #%d] Xbox back after %.0fs -- game kept running"
                % (c.id, time.monotonic() - self.disconnected_at))
        self.disconnected_at = None
        log("[client #%d] connected from %s" % (c.id, c.addr))
        threading.Thread(target=self._reader, args=(c,), daemon=True).start()

    def _on_client_closed(self, cid):
        if not self.client or self.client.id != cid:
            return
        log("[client #%d] disconnected" % cid)
        self.client.close()
        self.client = None
        if self.title or self.cap or self.login_proc:
            self.disconnected_at = time.monotonic()
            log("Xbox gone -- keeping game/login alive for %ds" % self.a.grace_secs)

    def _on_cmd(self, cid, line):
        if not self.client or self.client.id != cid:
            return
        f = line.split("\t")
        cmd = f[0].strip().upper()
        if cmd != "PING":
            log("<- #%d %s" % (cid, line.replace("\t", " | ")))
        if cmd == "HELLO":
            log("[client #%d] HELLO version=%s" % (cid, f[1] if len(f) > 1 else "?"))
            self.client.send("STATE", *self.state)
            if self.login_info:
                self.client.send("LOGIN", *self.login_info)
        elif cmd == "PING":
            self._pong()
        elif cmd == "LIST":
            self._cmd_list()
        elif cmd == "PLAY":
            self._cmd_play(f[1].strip() if len(f) > 1 else "")
        elif cmd == "STOP":
            self._cmd_stop("pedido do Xbox")
        elif cmd == "PAUSE":
            self._cmd_pause()
        elif cmd:
            log("[client #%d] unknown command %r" % (cid, cmd))

    def _pong(self):
        try:
            with self.client.lock:
                self.client.sock.sendall(b"PONG\n")
        except OSError:
            self.client.close()

    # ---------------- LIST / catalog ----------------
    def _catalog_stale(self):
        if not self.catalog:
            return True
        return time.time() - self.catalog.get("fetched_at", 0) > self.a.catalog_max_age

    def _cmd_list(self):
        if self.catalog and self.catalog.get("games"):
            self._send_list()
            if self._catalog_stale() or self.catalog_failed:
                log("catalog %s -- refreshing in background"
                    % ("failed last time" if self.catalog_failed else "older than max age"))
                self._fetch_catalog()
            return
        self.list_wanted = True
        if self.login_proc:
            self.set_state("LOGIN", self.state[1], force=True)
            return
        self._fetch_catalog()
        if self.state[0] != "CATALOG":
            self.state_before_catalog = self.state
        self.set_state("CATALOG", "Carregando catalogo...")

    def _send_list(self):
        games = self.catalog["games"]
        by_id = {g["id"]: g for g in games}
        rec = [t for t in self.recents if t in by_id]
        recset = set(rec)
        rest = sorted((g for g in games if g["id"] not in recset),
                      key=lambda g: ((g["name"] or g["id"]).casefold(), g["id"]))
        rows = [("GAME", t, ascii_fold(by_id[t]["name"] or t), "1") for t in rec]
        rows += [("GAME", g["id"], ascii_fold(g["name"] or g["id"]), "0") for g in rest]
        rows.append(("END",))
        if self.client:
            self.client.send_lines(rows)
        log("sent catalog: %d games (%d recent)" % (len(rows) - 1, len(rec)))

    def _fetch_catalog(self):
        if self.catalog_fetching:
            return
        self.catalog_fetching = True
        threading.Thread(target=self._catalog_worker, daemon=True).start()

    def _catalog_worker(self):
        log("running: %s games" % self.a.greennx)
        try:
            r = subprocess.run([self.a.greennx, "games"], stdin=subprocess.DEVNULL,
                               capture_output=True, text=True, timeout=300,
                               errors="replace")
            out = (r.stdout or "") + "\n" + (r.stderr or "")
            games, seen = [], set()
            for line in (r.stdout or "").splitlines():
                m = GAME_LINE_RE.match(line)
                if m and m.group(1) not in seen:
                    seen.add(m.group(1))
                    games.append({"id": m.group(1), "name": ascii_fold(m.group(2) or "")})
            if r.returncode == 0 and games:
                self.ev.put(("catalog_done", True, games, None, False))
            else:
                err = ([l for l in (r.stderr or "").splitlines() if l.strip()] or
                       ["exit %d, %d games" % (r.returncode, len(games))])[-1]
                self.ev.put(("catalog_done", False, None, err, bool(AUTH_RE.search(out))))
        except Exception as e:
            self.ev.put(("catalog_done", False, None, str(e), False))

    def _on_catalog_done(self, ok, games, err, auth):
        self.catalog_fetching = False
        if ok:
            self.catalog = {"fetched_at": time.time(), "games": games}
            write_json_atomic(self.a.catalog_cache, self.catalog)
            self.catalog_failed = False
            log("catalog refreshed: %d games -> %s" % (len(games), self.a.catalog_cache))
            if self.list_wanted:
                self.list_wanted = False
                self._restore_after_catalog()
                self._send_list()
            return
        self.catalog_failed = True
        log("catalog fetch FAILED: %s%s" % (err, " (auth)" if auth else ""))
        if auth and not self.title:
            self._start_login(("list",) if self.list_wanted else None)
        elif self.list_wanted:
            self.list_wanted = False
            self.set_state("ERROR", "Falha ao carregar catalogo: " + ascii_fold(err, 40))

    def _restore_after_catalog(self):
        if self.state[0] in ("CATALOG", "LOGIN"):
            prev = self.state_before_catalog
            if not prev or prev[0] in ("ERROR", "CATALOG", "LOGIN"):
                prev = ("IDLE", "Pronto")
            self.set_state(*prev)
        self.state_before_catalog = None

    # ---------------- login ----------------
    def _start_login(self, after):
        if after and (not self.after_login or after[0] == "play"):
            self.after_login = after
        if self.login_proc:
            return
        log("Microsoft login needed -- running: %s login" % self.a.greennx)
        try:
            proc = subprocess.Popen([self.a.greennx, "login"], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        except OSError as e:
            self.after_login = None
            self.set_state("ERROR", "Nao consegui rodar greennx login: %s" % e)
            return
        self.login_proc = proc
        self.login_info = None
        if self.state[0] not in ("CATALOG", "LOGIN"):
            self.state_before_catalog = None
        self.set_state("LOGIN", "Gerando codigo de login...")
        threading.Thread(target=self._login_worker, args=(proc,), daemon=True).start()

    def _login_worker(self, proc):
        text, found = "", False
        while True:
            chunk = proc.stdout.read1(4096) if hasattr(proc.stdout, "read1") else proc.stdout.read(1)
            if not chunk:
                break
            s = chunk.decode("utf-8", "replace")
            text += s
            for l in s.splitlines():
                if l.strip(" ."):
                    log("[greennx login] %s" % l.strip())
            if not found:
                m = DEVICE_CODE_RE.search(text)
                if m:
                    found = True
                    self.ev.put(("login_code", proc, m.group(1), m.group(2).upper()))
        rc = proc.wait()
        self.ev.put(("login_done", proc, rc, text[-400:]))

    def _on_login_code(self, proc, url, code):
        if proc is not self.login_proc:
            return
        self.login_info = (url, code)
        log("=" * 60)
        log("LOGIN NEEDED: open %s and enter code %s" % (url, code))
        log("=" * 60)
        short = re.sub(r"^https?://(www\.)?", "", url).rstrip("/")
        self.set_state("LOGIN", "Entre em %s e digite %s" % (short, code))
        if self.client:
            self.client.send("LOGIN", url, code)

    def _on_login_done(self, proc, rc, tail):
        if proc is not self.login_proc:
            return
        self.login_proc = None
        self.login_info = None
        after, self.after_login = self.after_login, None
        if rc == 0 and "Signed in" in tail:
            log("login OK (Signed in!)")
            self.last_login_ok = time.monotonic()
            self.catalog_failed = True     # tokens changed: refresh catalog when asked
            if after and after[0] == "play":
                self._begin_play(after[1], resume=True)
                self.play_after_login = after[1]
            elif after and after[0] == "list" or self.list_wanted:
                self.list_wanted = True
                self._fetch_catalog()
                self.set_state("CATALOG", "Carregando catalogo...")
            else:
                self.set_state("IDLE", "Login concluido")
        else:
            reason = "codigo expirou" if re.search(r"expired|Timed out", tail, re.I) \
                else "greennx saiu com codigo %d" % rc
            log("login FAILED: %s" % reason)
            self.list_wanted = False
            self.title = None
            self.set_state("ERROR", "Login falhou: " + reason)

    def _cancel_login(self):
        if self.login_proc:
            log("cancelling greennx login")
            try:
                os.killpg(self.login_proc.pid, signal.SIGTERM)
            except OSError:
                pass
        self.login_proc = None
        self.login_info = None
        self.after_login = None

    # ---------------- PLAY / STOP ----------------
    def _cmd_play(self, title):
        if not TITLE_RE.match(title):
            self.set_state("ERROR", "Jogo invalido", force=True)
            return
        title = title.upper()
        active = self.cap or self.start_pending or self.restart_at or self.killing
        was_paused, self.paused = self.paused, False
        if self.title == title and active and self.state[0] != "ERROR":
            if was_paused:
                log("PLAY %s: resuming the paused game (same session, no restart)" % title)
            else:
                log("PLAY %s: already running -- resending state" % title)
            self.client.send("STATE", *self.state)
            return
        if was_paused and self.title:
            log("PLAY %s: replaces the paused %s (stopped first)" % (title, self.title))
        self.recents = [title] + [t for t in self.recents if t != title]
        self.recents = self.recents[:10]
        st = read_json(self.a.state_file, {})
        st["recent"] = self.recents
        st["last_title"] = title
        try:
            write_json_atomic(self.a.state_file, st)
        except OSError as e:
            log("could not write state file: %s" % e)
        if self.login_proc:
            self.title = title
            self._start_login(("play", title))
            self.client.send("STATE", *self.state)
            return
        self._begin_play(title)

    def _begin_play(self, title, resume=False):
        self.restarts.clear()
        self.reconnecting = False
        self.restart_at = None
        self.play_after_login = None
        self.title = title
        self._stop_capture()
        self.start_pending = True
        self.set_state("STARTING", "Iniciando %s..." % self.game_name(title))
        self._maybe_start()

    def _cmd_pause(self):
        """v0.19.3: Back+Start on the Xbox -- the game stays open."""
        active = self.cap or self.start_pending or self.restart_at or self.killing
        if not self.title or not active:
            log("PAUSE with no game running -- ignored")
            return
        self.paused = True
        log("PAUSE %s: Xbox in the menu, game kept open (no auto-restart while paused)" % self.title)

    def _end_paused(self, reason):
        """v0.19.3: the paused game died -- nobody is playing, no new session."""
        log("paused game %s ended (%s) -- not restarting, the Xbox is in its menu" % (self.title, reason))
        self._stop_capture()
        self.title = None
        self.paused = False
        self.start_pending = False
        self.restart_at = None
        self.reconnecting = False
        self.set_state("IDLE", "Sessao do xCloud encerrada: %s" % ascii_fold(reason, 45), force=True)

    def _cmd_stop(self, why):
        log("STOP (%s)" % why)
        self.paused = False
        self.title = None
        self.start_pending = False
        self.restart_at = None
        self.reconnecting = False
        self.list_wanted = False
        self._cancel_login()
        self._stop_capture()
        self.set_state("IDLE", "Parado", force=True)

    # ---------------- capture process ----------------
    def _maybe_start(self):
        if (self.start_pending and self.killing == 0 and self.cap is None
                and self.login_proc is None and self.title):
            self.start_pending = False
            self._start_capture()

    def _start_capture(self):
        title = self.title
        port = self.a.video_port
        deadline = time.monotonic() + 10
        while not port_free(port):
            if time.monotonic() > deadline:
                log("video port %d is busy (another capture-rtp?) -- refusing to start" % port)
                self._give_up("Porta de video %d ocupada no PC" % port)
                return
            time.sleep(0.5)
        os.makedirs(self.a.log_dir, exist_ok=True)
        ts = time.strftime("%Y%m%d-%H%M%S")
        log_path = os.path.join(self.a.log_dir, "capture-%s-%s.log" % (title, ts))
        cmd = [self.a.capture_bin, title, "--tcp-out", str(port),
               "--seconds", str(self.a.seconds), "--no-file", "--kbps", str(self.a.kbps)]
        cmd += self.a.extra_arg
        log("starting: %s  (cwd %s, log %s)" % (" ".join(cmd), self.a.repo, log_path))
        try:
            with open(log_path, "ab") as lf:
                proc = subprocess.Popen(cmd, cwd=self.a.repo, stdin=subprocess.DEVNULL,
                                        stdout=lf, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        except OSError as e:
            self._give_up("Nao consegui iniciar capture-rtp: %s" % e)
            return
        self.gen += 1
        run = CaptureRun(self.gen, title, proc, log_path)
        self.cap = run
        log("capture-rtp pid %d (gen %d)" % (proc.pid, run.gen))
        threading.Thread(target=self._tail, args=(run,), daemon=True).start()
        self._update_compat_log(log_path, proc.pid)

    def _update_compat_log(self, log_path, our_pid):
        c = self.a.compat_log
        if not c:
            return
        others = [p for p in procs_named("capture-rtp") if p != our_pid]
        if others:
            log("NOT touching %s: capture-rtp not started by the launcher is running (pid %s)"
                % (c, ",".join(map(str, others))))
            return
        try:
            if os.path.lexists(c) and not os.path.islink(c):
                old = c + ".old"
                os.replace(c, old)
                log("moved previous %s to %s" % (c, old))
            tmp = c + ".launcher-tmp"
            if os.path.lexists(tmp):
                os.remove(tmp)
            os.symlink(log_path, tmp)
            os.replace(tmp, c)
            log("%s -> %s" % (c, log_path))
        except OSError as e:
            log("could not update %s: %s" % (c, e))

    INTERESTING = re.compile(r"^\[|ERROR|error|failed|what\(\)|terminate|no saved login|"
                             r"not signed in|sign in again|Segmentation")

    def _tail(self, run):
        """Follow the per-session log file and feed interesting lines to the loop.
        The child writes to the file directly so the log survives a launcher
        crash and libpeer's DEBUG firehose never goes through a pipe."""
        def emit(raw):
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if self.INTERESTING.search(line):
                self.ev.put(("cap_line", run.gen, line))
        try:
            with open(run.log_path, "rb") as f:
                buf = b""
                done = False
                while True:
                    chunk = f.read(1 << 16)
                    if chunk:
                        buf += chunk
                        *lines, buf = buf.split(b"\n")
                        for l in lines:
                            emit(l)
                        continue
                    if done:
                        break
                    if run.proc.poll() is not None:
                        done = True      # one more read to drain what's left
                        continue
                    time.sleep(0.1)
                if buf:
                    emit(buf)
        except OSError as e:
            log("tail of %s failed: %s" % (run.log_path, e))
        rc = run.proc.wait()
        self.ev.put(("cap_exit", run.gen, rc))

    def _phase(self, code, text):
        """State change coming from the capture log (respects reconnect mode)."""
        if self.reconnecting and code == "STARTING":
            code, text = "RECONNECTING", "Reconectando: " + text
        self.set_state(code, text)

    def _on_cap_line(self, gen, line):
        run = self.cap
        if not run or run.gen != gen or run.stopping:
            return
        now = time.monotonic()
        run.last_line_t = now
        tag = "[cap:%s]" % run.title
        if line.startswith("[XCLOUD360-STATE]"):
            try:
                obj = json.loads(line.split(" ", 1)[1])
            except (ValueError, IndexError):
                return
            st = obj.get("state", "")
            if st != run.sess_state:
                log("%s xCloud session state: %s" % (tag, line.split(" ", 1)[1][:300]))
                run.sess_state = st
            if st == "WaitingForResources":
                pos, eta = queue_info(obj)
                self.set_state("QUEUE", queue_text(pos, eta))
            elif st in ("New", "Provisioning", "ReadyToConnect"):
                self._phase("STARTING", "Preparando servidor...")
            elif st == "Provisioned":
                self._phase("STARTING", "Conectando video...")
            elif st in ("Failed", "Error"):
                det = obj.get("errorDetails") or {}
                if isinstance(det, dict):
                    det = det.get("code") or det.get("message") or ""
                run.last_error = "Sessao xCloud falhou %s" % ascii_fold(str(det or ""), 40)
            return
        if line.startswith("[cap] t+"):
            m = AU_RE.search(line)
            if m:
                au = int(m.group(1))
                if au > run.au:
                    run.au, run.au_t = au, now
                    if run.connected and not run.streaming:
                        run.streaming = True
                        self.reconnecting = False
                        self.play_after_login = None
                        log("%s video flowing (%d AUs) -> STREAMING" % (tag, au))
                        self.set_state("STREAMING", self.game_name(run.title))
            if now - run.last_stat_log >= 30:
                run.last_stat_log = now
                log("%s %s" % (tag, line[:200]))
            return
        if line.startswith("[xbox]"):
            # Telemetria completa (a cada 2s) num arquivo à parte.
            try:
                with open("/tmp/xbox-stat.log", "a") as f:
                    f.write("%s %s\n" % (time.strftime("%H:%M:%S"), line[:300]))
            except OSError:
                pass
            if now - run.last_xbox_log >= 30:
                run.last_xbox_log = now
                log("%s %s" % (tag, line[:200]))
            return
        if "sctp not connected" in line:
            run.sctp_errs += 1
            return
        if line.startswith("[sess] state:") or line.startswith("[ice]") or line.startswith("[input"):
            pass
        else:
            log("%s %s" % (tag, line[:300]))
        if line.startswith("[cap] fetching streaming credentials"):
            self._phase("STARTING", "Autenticando na Microsoft...")
        elif line.startswith("[cap] endpoint host"):
            run.creds_ok = True
        elif line.startswith("[cap] CONNECTED"):
            run.connected = True
            run.au_t = now
            self._phase("STARTING", "Aguardando video...")
        if AUTH_RE.search(line) or ("HTTP 401" in line and not run.creds_ok):
            run.auth_fail = True
        for rx, label in CAP_ERROR_RES:
            m = rx.search(line)
            if m:
                det = m.group(1) if m.groups() else ""
                run.last_error = (label + (": " + ascii_fold(det, 40) if det else ""))
                break

    def _on_cap_exit(self, gen, rc):
        run = self.cap
        if not run or run.gen != gen:
            return
        self.cap = None
        log("[cap:%s] capture-rtp exited unexpectedly rc=%s after %.0fs (AUs=%d)"
            % (run.title, rc, time.monotonic() - run.started, run.au))
        if self.paused:
            self._end_paused(run.last_error or "capture-rtp saiu (codigo %s)" % rc)
            return
        if run.auth_fail:
            if self.play_after_login == run.title:
                # we just signed in for this very PLAY and it still fails:
                # don't loop login -> fail -> login forever.
                self._give_up("Login aceito mas o xCloud recusou as credenciais")
                return
            log("capture-rtp failed on credentials -> login flow")
            self.reconnecting = False
            self._start_login(("play", run.title))
            return
        self._auto_restart(run.last_error or "capture-rtp saiu (codigo %s)" % rc)

    def _stop_capture(self):
        run = self.cap
        if not run:
            return
        self.cap = None
        run.stopping = True
        self.killing += 1
        log("stopping capture-rtp pid %d (%s)" % (run.proc.pid, run.title))
        threading.Thread(target=self._killer, args=(run,), daemon=True).start()

    def _kill_run(self, run):
        try:
            os.killpg(run.proc.pid, signal.SIGINT)
        except OSError:
            pass
        try:
            rc = run.proc.wait(self.a.stop_timeout)
        except subprocess.TimeoutExpired:
            log("capture-rtp pid %d ignored SIGINT for %ds -> SIGKILL"
                % (run.proc.pid, self.a.stop_timeout))
            try:
                os.killpg(run.proc.pid, signal.SIGKILL)
            except OSError:
                pass
            rc = run.proc.wait()
        return rc

    def _killer(self, run):
        rc = self._kill_run(run)
        self.ev.put(("stopped", run.gen, rc))

    def _on_stopped(self, gen, rc):
        self.killing -= 1
        log("capture-rtp gen %d stopped (rc=%s)" % (gen, rc))
        self._maybe_start()

    # ---------------- watchdog ----------------
    def _auto_restart(self, reason):
        if self.paused and self.title:
            self._end_paused(reason)
            return
        now = time.monotonic()
        while self.restarts and now - self.restarts[0] > self.a.restart_window:
            self.restarts.popleft()
        self._stop_capture()
        if not self.title:
            return
        if len(self.restarts) >= self.a.max_restarts:
            log("watchdog: %d restarts in %ds already -- giving up (%s)"
                % (len(self.restarts), self.a.restart_window, reason))
            self._give_up("%s (%d tentativas falharam)" % (reason, len(self.restarts)))
            return
        self.restarts.append(now)
        n = len(self.restarts)
        delay = self.a.backoff[min(n - 1, len(self.a.backoff) - 1)]
        self.reconnecting = True
        self.start_pending = False
        self.restart_at = now + delay
        log("watchdog: %s -> restart %d/%d in %.0fs" % (reason, n, self.a.max_restarts, delay))
        self.set_state("RECONNECTING", "Reconectando (%d/%d): %s"
                       % (n, self.a.max_restarts, ascii_fold(reason, 45)))

    def _give_up(self, reason):
        self._stop_capture()
        self.paused = False
        self.title = None
        self.start_pending = False
        self.restart_at = None
        self.reconnecting = False
        self.set_state("ERROR", reason, force=True)

    def _on_tick(self):
        now = time.monotonic()
        if self.restart_at and now >= self.restart_at:
            self.restart_at = None
            if self.title and self.paused:
                # v0.19.3: died mid-game, then Back+Start: no new session in the menu
                self._end_paused("a conexao tinha caido")
            elif self.title:
                log("watchdog: restarting %s" % self.title)
                self.start_pending = True
                self._maybe_start()
        run = self.cap
        if run and not run.stopping:
            if run.sctp_errs != run.sctp_logged and now - run.sctp_log_t >= 10:
                log("[cap:%s] %d 'sctp not connected' errors so far" % (run.title, run.sctp_errs))
                run.sctp_logged, run.sctp_log_t = run.sctp_errs, now
            if run.connected and now - run.au_t > self.a.stall_secs:
                log("watchdog: video AU counter stuck at %d for %.0fs" % (run.au, now - run.au_t))
                self._auto_restart("Video parou (%ds sem quadros)" % self.a.stall_secs)
            elif not run.connected and now - run.last_line_t > self.a.silence_secs:
                log("watchdog: capture-rtp silent for %.0fs before connecting" % (now - run.last_line_t))
                self._auto_restart("capture-rtp travou antes de conectar")
        if (self.client is None and self.disconnected_at is not None
                and now - self.disconnected_at > self.a.grace_secs):
            self.disconnected_at = None
            if self.title or self.cap or self.login_proc or self.restart_at:
                log("Xbox did not come back in %ds -- stopping game" % self.a.grace_secs)
                self._cmd_stop("Xbox desconectado")

    # ---------------- shutdown ----------------
    def _shutdown(self):
        log("launcher shutting down")
        self._cancel_login()
        run, self.cap = self.cap, None
        if run:
            run.stopping = True
            log("stopping capture-rtp pid %d" % run.proc.pid)
            self._kill_run(run)
        if self.client:
            self.client.send("STATE", "IDLE", "Launcher do PC encerrado")
            self.client.close()
        log("bye")


def main():
    ap = argparse.ArgumentParser(description="XCloud360 launcher daemon (Xbox <-> capture-rtp)")
    ap.add_argument("--port", type=int, default=9002, help="control port for the Xbox")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--video-port", type=int, default=9000, help="capture-rtp --tcp-out port")
    ap.add_argument("--repo", default=str(DEFAULT_REPO), help="repo root (capture-rtp cwd)")
    ap.add_argument("--capture-bin", default=None,
                    help="default: <repo>/tools/xcloud-capture/rtp/capture-rtp")
    ap.add_argument("--greennx", default=str(HOME / "green-nx/build/pc/greennx"))
    ap.add_argument("--state-file", default=str(HOME / ".config/xcloud360/launcher.json"))
    ap.add_argument("--catalog-cache", default=str(HOME / ".config/xcloud360/catalog.json"))
    ap.add_argument("--log-dir", default="/tmp/xcloud-launcher")
    ap.add_argument("--compat-log", default="/tmp/capture-rtp.log",
                    help="symlink kept pointing at the latest session log ('' = off)")
    ap.add_argument("--seconds", type=int, default=21600)
    ap.add_argument("--kbps", type=int, default=1500)
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra argument for capture-rtp (repeatable), e.g. --extra-arg=--drop-nonref")
    ap.add_argument("--stall-secs", type=float, default=20)
    ap.add_argument("--silence-secs", type=float, default=180,
                    help="restart if capture-rtp prints nothing for this long before CONNECTED")
    ap.add_argument("--grace-secs", type=float, default=300,
                    help="keep the game running this long after the Xbox disconnects")
    ap.add_argument("--stop-timeout", type=float, default=15)
    ap.add_argument("--max-restarts", type=int, default=3)
    ap.add_argument("--restart-window", type=float, default=600)
    ap.add_argument("--backoff", default="3,10,30", help="seconds before restart 1,2,3...")
    ap.add_argument("--catalog-max-age", type=float, default=6 * 3600)
    a = ap.parse_args()
    a.capture_bin = a.capture_bin or os.path.join(a.repo, "tools/xcloud-capture/rtp/capture-rtp")
    a.backoff = [float(x) for x in a.backoff.split(",") if x.strip()] or [3.0]
    try:
        Launcher(a).run()
    except OSError as e:
        log("fatal: %s" % e)
        sys.exit(1)


if __name__ == "__main__":
    main()
