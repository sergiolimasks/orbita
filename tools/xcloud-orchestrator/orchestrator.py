#!/usr/bin/env python3
"""
XCloud360 — orchestrator device-code overlay + stream router.

Fase A (device-code overlay):
  1. Spawn `greennx login` como subprocess.
  2. Parse stdout pra achar URL + código (formato device-code MSA).
  3. Gera H.264 720p60 baseline live via ffmpeg drawtext.
  4. Serve na porta 9000 (mesmo wire do h264_feeder — length-prefix AU).
  5. Xbox conecta e renderiza tela com URL + código.
  6. Sergio autoriza no navegador.
  7. greennx login retorna → transição pra Fase B.

Fase B (stream router — best-effort):
  8. Spawn `greennx stream-test ASSASSINSCREEDODYSSEY`.
  9. Se sucesso: proxy stream pro Xbox (não implementado nesta sessão — dead
     media path bloqueia; capture-rtp falhou 2/2).
     Fallback: mostra tela "AUTORIZADO — stream bloqueado (dead media)".

Uso:
  python3 orchestrator.py                  # Fase A completa + fallback
  python3 orchestrator.py --static-only    # só tela estática pra teste TV
  python3 orchestrator.py --code TEST-1234 # bypass MSA, mostra código fake
"""
from __future__ import annotations
import argparse
import atexit
import os
import pathlib
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from typing import Optional

# Force line buffering so logs from subprocess pipes appear immediately.
try:
    sys.stdout.reconfigure(line_buffering=True)  # type: ignore[attr-defined]
    sys.stderr.reconfigure(line_buffering=True)  # type: ignore[attr-defined]
except Exception:
    pass

# ---------- constants ----------
PORT_DEFAULT = 9000
FPS = 60
GREENNX_BIN = os.path.expanduser("~/green-nx/build/pc/greennx")
CAPTURE_RTP_BIN = "/mnt/storage/projetos/XCloud360/tools/xcloud-capture/rtp/capture-rtp"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
STARTCODE_4 = b"\x00\x00\x00\x01"
STARTCODE_3 = b"\x00\x00\x01"

# ---------- device code parser ----------
# greennx CLI src/cli/main.cpp:26 emits exactly:
#   "\nTo sign in, open <URL>\nand enter code: <CODE>\n\nWaiting"
# The URL Microsoft returns is usually https://www.microsoft.com/link and the
# code is 8 hex chars in one group (no dash).
DEVICE_CODE_RE = re.compile(
    r"To sign in,\s+open\s+(\S+)\s+and\s+enter\s+code:\s+([A-Z0-9]+)",
    re.IGNORECASE | re.DOTALL,
)

# ---------- H.264 wire (mirror h264_feeder — reused verbatim) ----------
def scan_nals(buf: bytes):
    """Yield (nal_offset_incl_sc, sc_len, nal_type)."""
    i, n = 0, len(buf)
    out = []
    while i < n - 3:
        if buf[i] == 0 and buf[i + 1] == 0:
            if buf[i + 2] == 1:
                sc = 3
            elif i + 3 < n and buf[i + 2] == 0 and buf[i + 3] == 1:
                sc = 4
            else:
                i += 1; continue
            nal_start = i + sc
            if nal_start >= n:
                break
            out.append((i, sc, buf[nal_start] & 0x1F))
            i = nal_start + 1
        else:
            i += 1
    return out


def is_first_slice_of_picture(buf: bytes, nal_start: int) -> bool:
    if nal_start + 1 >= len(buf):
        return False
    return (buf[nal_start + 1] & 0x80) != 0


def segment_aus(data: bytes):
    """Segmenta Annex-B em AUs. Cada AU auto-contida (start-codes preservados)."""
    nals = scan_nals(data)
    if not nals:
        return []
    boundaries = [False] * len(nals)
    boundaries[0] = True
    seen_slice_in_current_au = False
    SLICE_TYPES = {1, 5}
    PREFIX_TYPES = {6, 7, 8, 9, 13, 14, 15, 16, 17, 18}
    for idx, (off, sc, ntype) in enumerate(nals):
        if idx == 0:
            if ntype in SLICE_TYPES:
                seen_slice_in_current_au = True
            continue
        new_au = False
        if ntype == 9:
            new_au = True
        elif ntype in PREFIX_TYPES and seen_slice_in_current_au:
            new_au = True
        elif ntype in SLICE_TYPES:
            nal_start = off + sc
            if is_first_slice_of_picture(data, nal_start):
                if seen_slice_in_current_au:
                    new_au = True
        if new_au:
            boundaries[idx] = True
            seen_slice_in_current_au = False
        if ntype in SLICE_TYPES:
            seen_slice_in_current_au = True
    start_offsets = [nals[i][0] for i, b in enumerate(boundaries) if b] + [len(data)]
    aus = []
    for i in range(len(start_offsets) - 1):
        aus.append(data[start_offsets[i]:start_offsets[i + 1]])
    return aus

# ---------- ffmpeg overlay generator ----------
def ffmpeg_escape(s: str) -> str:
    """Escape for ffmpeg drawtext text arg."""
    return s.replace("\\", "\\\\").replace(":", r"\:").replace("'", r"\'")

def build_drawtext_filter(lines: list[tuple[str, int, str, int]]) -> str:
    """lines = [(text, fontsize, color, y_pos), ...]"""
    parts = []
    for text, size, color, y in lines:
        esc = ffmpeg_escape(text)
        parts.append(
            f"drawtext=fontfile={FONT_PATH}:text='{esc}':"
            f"fontsize={size}:fontcolor={color}:x=(w-text_w)/2:y={y}"
        )
    return ",".join(parts)

def generate_overlay_h264(out_path: pathlib.Path, url: str, code: str,
                          status: str = "AGUARDANDO LOGIN", duration_s: int = 30) -> pathlib.Path:
    """Generate a looping H.264 overlay stream with device code."""
    lines = [
        ("XCloud360",          96, "0x58a6ff", 100),
        (status,               56, "white",    240),
        ("Acesse:",            40, "0x8b949e", 340),
        (url,                  44, "0xffa657", 400),
        ("Codigo:",            40, "0x8b949e", 500),
        (code,                 88, "0x3fb950", 560),
    ]
    vf = build_drawtext_filter(lines)
    cmd = [
        "ffmpeg", "-y", "-f", "lavfi",
        "-i", f"color=c=0x0d1117:s=1280x720:r={FPS}:d={duration_s}",
        "-vf", vf,
        "-c:v", "libx264", "-profile:v", "baseline", "-level:v", "3.1",
        "-pix_fmt", "yuv420p",
        "-g", str(FPS), "-keyint_min", str(FPS), "-sc_threshold", "0",
        "-bf", "0", "-x264-params", "no-cabac=1:ref=1:no-scenecut=1",
        "-tune", "zerolatency", "-b:v", "3000k",
        "-f", "h264", str(out_path),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {r.stderr[-500:]}")
    return out_path

# ---------- TCP server ----------
class OverlayServer:
    """
    Serves length-prefix H.264 AUs on a port. Content is HOT-SWAPPABLE
    via update_aus() — the next AU boundary uses the new stream. Loops
    forever.
    """
    def __init__(self, port: int = PORT_DEFAULT):
        self.port = port
        self.aus: list[bytes] = []
        self.aus_lock = threading.Lock()
        self.sock: Optional[socket.socket] = None
        self.stop_flag = threading.Event()

    def update_aus(self, aus: list[bytes]):
        with self.aus_lock:
            self.aus = aus
        print(f"[server] aus updated ({len(aus)} AUs)")

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", self.port))
        self.sock.listen(4)
        print(f"[server] listening 0.0.0.0:{self.port}")
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def _accept_loop(self):
        while not self.stop_flag.is_set():
            try:
                self.sock.settimeout(1.0)
                conn, addr = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve_client, args=(conn, addr), daemon=True).start()

    def _serve_client(self, conn: socket.socket, addr):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
        period = 1.0 / FPS
        frames = 0
        t0 = time.monotonic()
        print(f"[server] client {addr} connected")
        try:
            while not self.stop_flag.is_set():
                with self.aus_lock:
                    aus = list(self.aus)
                if not aus:
                    time.sleep(0.05)
                    continue
                for au in aus:
                    if self.stop_flag.is_set():
                        break
                    conn.sendall(struct.pack(">I", len(au)) + au)
                    frames += 1
                    # pace at 60fps
                    deadline = t0 + frames * period
                    slack = deadline - time.monotonic()
                    if slack > 0:
                        time.sleep(slack)
                if frames % 300 == 0:
                    el = time.monotonic() - t0
                    print(f"[server] {addr} sent={frames} eff_fps={frames/el:.1f}")
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            el = time.monotonic() - t0
            print(f"[server] {addr} gone after {frames} frames / {el:.1f}s ({e.__class__.__name__})")
        finally:
            try: conn.close()
            except Exception: pass

    def stop(self):
        self.stop_flag.set()
        if self.sock:
            try: self.sock.close()
            except Exception: pass

# ---------- greennx login driver ----------
def spawn_greennx_login(on_code_ready, on_authorized, on_failure):
    """Spawn `greennx login` and stream stdout.

    on_code_ready(url, code): called when device code appears.
    on_authorized(): called when login finishes successfully.
    on_failure(reason): called on error / timeout.
    """
    def _run():
        try:
            proc = subprocess.Popen(
                [GREENNX_BIN, "login"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
            )
        except Exception as e:
            on_failure(f"spawn failed: {e}")
            return

        captured = []
        code_found = False
        try:
            for line in proc.stdout:
                sys.stdout.write(f"[gnx] {line}")
                sys.stdout.flush()
                captured.append(line)
                if not code_found:
                    joined = "".join(captured[-15:])
                    m = DEVICE_CODE_RE.search(joined)
                    if m:
                        code_found = True
                        on_code_ready(m.group(1), m.group(2).strip().upper())
            rc = proc.wait()
            if rc == 0:
                on_authorized()
            else:
                on_failure(f"greennx exit={rc}")
        except Exception as e:
            on_failure(f"greennx error: {e}")

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t

# ---------- Fase A driver ----------
def phase_a(server: OverlayServer, workdir: pathlib.Path,
            static_only: bool = False, static_code: Optional[str] = None):
    """Fase A: overlay device-code até MSA autorizar."""
    initial = generate_overlay_h264(
        workdir / "boot.h264", "microsoft.com/link", "AGUARDANDO...",
        status="INICIANDO",
    )
    server.update_aus(segment_aus(initial.read_bytes()))

    if static_only:
        code = static_code or "TEST-1234"
        overlay = generate_overlay_h264(
            workdir / "static.h264",
            "microsoft.com/link", code, status="MODO ESTATICO",
        )
        server.update_aus(segment_aus(overlay.read_bytes()))
        print(f"[phaseA] STATIC mode — code={code}")
        return "static"

    state = {"result": None}
    ev_done = threading.Event()

    def on_code(url, code):
        print(f"[phaseA] DEVICE CODE: url={url}  code={code}")
        overlay = generate_overlay_h264(
            workdir / "device-code.h264", url, code, status="AGUARDANDO LOGIN",
        )
        server.update_aus(segment_aus(overlay.read_bytes()))

    def on_ok():
        print("[phaseA] AUTHORIZED")
        ok = generate_overlay_h264(
            workdir / "authorized.h264",
            "OK", "AUTORIZADO", status="LOGIN OK",
        )
        server.update_aus(segment_aus(ok.read_bytes()))
        state["result"] = "authorized"
        ev_done.set()

    def on_fail(reason):
        print(f"[phaseA] FAIL: {reason}")
        err = generate_overlay_h264(
            workdir / "error.h264", "-", "ERRO", status=f"FALHA: {reason[:40]}",
        )
        server.update_aus(segment_aus(err.read_bytes()))
        state["result"] = f"error:{reason}"
        ev_done.set()

    spawn_greennx_login(on_code, on_ok, on_fail)
    # wait up to 15min for user to authorize (device code TTL is usually 15m)
    ev_done.wait(timeout=15 * 60)
    return state["result"] or "timeout"

# ---------- Fase B driver (real streaming via capture-rtp) ----------
def phase_b(server: OverlayServer, workdir: pathlib.Path,
            title: str = "ASSASSINSCREEDODYSSEY", port: int = PORT_DEFAULT):
    """Fase B: real xCloud streaming via capture-rtp.

    Sequence:
      1. Show "CONECTANDO..." overlay on port 9000.
      2. Spawn capture-rtp with --tcp-out <internal port>.
      3. Wait for capture-rtp to bind and start streaming.
      4. Stop the overlay server, freeing port 9000.
      5. Rebind capture-rtp's TCP-out to 9000 by respawning it with
         --tcp-out 9000 directly. (Xbox will reconnect from its retry loop.)

    Simpler alternative implemented here: just stop OverlayServer, then
    spawn capture-rtp directly on port 9000. Xbox client reconnects when its
    read fails, and grabs the fresh stream on the next attempt.
    """
    print(f"[phaseB] preparing real stream for title={title}")

    # 1. Show CONNECTING screen while we bring up the real stream.
    connecting = generate_overlay_h264(
        workdir / "connecting.h264",
        f"Jogo: {title}", "AGUARDE",
        status="CONECTANDO...",
    )
    server.update_aus(segment_aus(connecting.read_bytes()))

    # 2. Sanity-check capture-rtp binary is built.
    if not os.path.isfile(CAPTURE_RTP_BIN) or not os.access(CAPTURE_RTP_BIN, os.X_OK):
        print(f"[phaseB] FAIL: capture-rtp not built at {CAPTURE_RTP_BIN}")
        err = generate_overlay_h264(
            workdir / "no-capture.h264", "capture-rtp missing", "BUILD",
            status="ERRO: capture-rtp nao existe",
        )
        server.update_aus(segment_aus(err.read_bytes()))
        return "no_binary"

    # 3. Stop overlay server to free port 9000.
    print(f"[phaseB] stopping overlay server (port {port}) to hand off to capture-rtp")
    server.stop()
    # Give kernel time to release the SO_REUSEADDR'd socket.
    time.sleep(1.5)

    # 4. Spawn capture-rtp directly on the target port. --seconds is a hard
    #    cap; use 3600 (1h) so the stream stays up long enough for a session.
    log_path = pathlib.Path("/tmp/capture-rtp.log")
    log_fd = open(log_path, "w", buffering=1)
    cmd = [
        CAPTURE_RTP_BIN, title,
        "--tcp-out", str(port),
        "--seconds", "3600",
        "--no-file",
        "--out", str(workdir),
    ]
    print(f"[phaseB] spawn: {' '.join(cmd)}")
    print(f"[phaseB] capture-rtp log: {log_path}")
    proc = subprocess.Popen(cmd, stdout=log_fd, stderr=subprocess.STDOUT)

    # 5. Wait until capture-rtp binds the TCP port. Poll for up to 60s
    #    (session provisioning + DTLS handshake takes 10-30s in practice).
    print(f"[phaseB] waiting up to 60s for capture-rtp to bind :{port}")
    bound = False
    for _ in range(120):  # 60s / 0.5s
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect(("127.0.0.1", port))
            s.close()
            bound = True
            break
        except (ConnectionRefusedError, socket.timeout, OSError):
            pass
        if proc.poll() is not None:
            print(f"[phaseB] capture-rtp died early (exit={proc.returncode})")
            print(f"[phaseB] check {log_path} for details")
            return "died"
        time.sleep(0.5)

    if not bound:
        print(f"[phaseB] capture-rtp never bound :{port}")
        try:
            proc.terminate()
        except Exception:
            pass
        return "no_bind"

    print(f"[phaseB] capture-rtp streaming on :{port} — Xbox will reconnect and receive real stream")
    print(f"[phaseB] PID={proc.pid} — leaving it running")
    # Keep this attached so orchestrator's main loop can wait/monitor.
    globals()["_CAPTURE_PROC"] = proc
    return "streaming"

# ---------- main ----------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT_DEFAULT)
    ap.add_argument("--static-only", action="store_true",
                    help="Não roda greennx — só mostra tela estática")
    ap.add_argument("--code", type=str, default=None,
                    help="Código estático a mostrar em --static-only mode")
    ap.add_argument("--phase-b", action="store_true",
                    help="Após Fase A (autorizado), transita pra streaming real via capture-rtp")
    ap.add_argument("--workdir", type=str,
                    default="/mnt/storage/projetos/XCloud360/tools/xcloud-orchestrator/run")
    args = ap.parse_args()

    workdir = pathlib.Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    server = OverlayServer(port=args.port)
    atexit.register(server.stop)
    server.start()

    print("[main] Fase A — device-code overlay")
    result_a = phase_a(server, workdir,
                       static_only=args.static_only,
                       static_code=args.code)
    print(f"[main] Fase A result: {result_a}")

    if args.phase_b and result_a == "authorized":
        print("[main] Fase B — real xCloud stream via capture-rtp")
        result_b = phase_b(server, workdir, port=args.port)
        print(f"[main] Fase B result: {result_b}")
        if result_b == "streaming":
            print("[main] streaming ativo. Ctrl+C pra encerrar.")
            proc = globals().get("_CAPTURE_PROC")
            try:
                while True:
                    if proc and proc.poll() is not None:
                        print(f"[main] capture-rtp exited (rc={proc.returncode})")
                        break
                    time.sleep(2)
            except KeyboardInterrupt:
                print("\n[main] shutdown — killing capture-rtp")
                if proc:
                    try: proc.terminate()
                    except Exception: pass
            return

    # Keep serving until interrupted
    print("[main] server continua rodando — Ctrl+C pra sair")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        print("\n[main] shutdown")

if __name__ == "__main__":
    main()
