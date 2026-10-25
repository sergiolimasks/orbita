#!/usr/bin/env python3
"""Fake capture-rtp for launcher tests. Prints realistic log lines.

Usage like the real one: fake_capture.py TITLE --tcp-out PORT --seconds N --no-file --kbps K

Behaviour per run comes from $FAKE_CAP_DIR/<TITLE>.modes (one mode per line,
the first line is consumed by each run; empty/missing file -> "ok"):
  ok            queue-less start, then AUs grow forever
  queue:N       N seconds in WaitingForResources (position counts down), then ok
  stall:N       stream N seconds, then the AU counter freezes (+ sctp errors)
  crash:N       stream N seconds, then die like an uncaught exception (rc 134)
  failstart     session fails before connecting (rc 9)
  authfail      credential fetch fails if $FAKE_AUTH_FILE is missing (rc 134)
  hang          like ok but ignores SIGINT (launcher must SIGKILL)
If $FAKE_AUTH_FILE is set and the file does not exist, every mode behaves like authfail.
"""
import json
import os
import signal
import socket
import sys
import time

title = sys.argv[1]
args = sys.argv[2:]
port = int(args[args.index("--tcp-out") + 1]) if "--tcp-out" in args else 0
secs = int(args[args.index("--seconds") + 1]) if "--seconds" in args else 7200
ctl = os.environ.get("FAKE_CAP_DIR", "/tmp")
tick = float(os.environ.get("FAKE_TICK", "1"))

mode = "ok"
mf = os.path.join(ctl, title + ".modes")
if os.path.exists(mf):
    lines = [l.strip() for l in open(mf) if l.strip()]
    if lines:
        mode = lines[0]
        with open(mf, "w") as f:
            f.write("\n".join(lines[1:]) + ("\n" if lines[1:] else ""))
kind, _, n = mode.partition(":")
n = int(n or 0)
with open(os.path.join(ctl, "runs.log"), "a") as f:
    f.write("%s pid=%d title=%s mode=%s argv=%s cwd=%s\n" % (
        time.strftime("%H:%M:%S"), os.getpid(), title, mode, " ".join(sys.argv[1:]), os.getcwd()))

stopping = False


def on_int(*_):
    global stopping
    stopping = True


if kind == "hang":
    signal.signal(signal.SIGINT, signal.SIG_IGN)
else:
    signal.signal(signal.SIGINT, on_int)


def p(s, err=False):
    (sys.stderr if err else sys.stdout).write(s + "\n")
    (sys.stderr if err else sys.stdout).flush()


def state(st, extra=None):
    d = {"errorDetails": None, "serverPlatform": "XBOX", "state": st}
    d.update(extra or {})
    p("[XCLOUD360-STATE] " + json.dumps(d, separators=(",", ":")), err=True)
    p("[sess] state: %d" % {"WaitingForResources": 2, "Provisioning": 1,
                            "ReadyToConnect": 3, "Provisioned": 4}.get(st, 0))


def sleep(t):
    end = time.time() + t * tick
    while time.time() < end:
        if stopping:
            p("[cap] stopping...")
            p("[cap] done. output: tools/xcloud-capture/output/%s-rtp-fake" % title)
            sys.exit(0)
        time.sleep(0.05)


p("[cap] title=%s  out=tools/xcloud-capture/output/%s-rtp-%s  seconds=%d"
  % (title, title, time.strftime("%Y%m%d-%H%M%S"), secs))
p("[cap] fetching streaming credentials...")
sleep(0.5)
auth = os.environ.get("FAKE_AUTH_FILE")
if kind == "authfail" or (auth and not os.path.exists(auth)):
    p("terminate called after throwing an instance of 'std::runtime_error'", err=True)
    p("  what():  saved login expired; please sign in again", err=True)
    sys.exit(134)
p("[cap] endpoint host: https://brs.core.gssv-play-prod.xboxlive.com")
p("[cap] fetching passport token...")
p("[cap] peer_init()")
p("")
p("[cap] ===== SESSION ATTEMPT 1/2 =====")
p("INFO\t/home/x/libpeer/src/agent.c\t41\tcreate IPv4 UDP socket: 7")
p("[cap] cleanup_stale_sessions()")
p("[cap] start_cloud(%s), tier=720p" % title)
if kind == "failstart":
    state("Failed", {"errorDetails": {"code": "TitleNotAvailable", "message": "nope"}})
    p("session failed: {\"code\":\"TitleNotAvailable\"}", err=True)
    p("[cap] all 2 attempts failed — giving up", err=True)
    sys.exit(9)
if kind == "queue":
    for i in range(n):
        left = n - i
        state("WaitingForResources", {"waitingTimes": {
            "estimatedTotalWaitTimeInSeconds": left * 40, "positionInQueue": left * 3}})
        sleep(1)
state("ReadyToConnect")
p("[sess] sending connect (passport)")
for _ in range(2):
    state("Provisioning")
    sleep(1)
state("Provisioned")
p("[sess] Provisioned — moving to SDP exchange")
p("[sdp] creating local offer")
for _ in range(20):
    p("DEBUG\t/home/x/libpeer/src/agent.c\t308\tlocal description noise")
p("[sdp] exchange_sdp -> xCloud")
sleep(0.5)
p("[cap] ICE connected — starting 12s DTLS window")
p("[cap] session 1 SUCCESS — DTLS up, proceeding to capture")
lsock = None
if port:
    lsock = socket.socket()
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("0.0.0.0", port))
    lsock.listen(1)
    p("[tcp] listening on 0.0.0.0:%d (length-prefix Annex-B AU, queue<=3 age<=150ms)" % port)
p("[cap] CONNECTED, capturing %d seconds of RTP..." % secs)
au = 0
byt = 0
for s in range(1, secs + 1):
    sleep(1)
    frozen = kind == "stall" and s > n
    if not frozen:
        au += 60
        byt += 60 * 1300
    else:
        for _ in range(3):
            p("ERROR\t/home/x/libpeer/src/peer_connection.c\t411\tsctp not connected")
    p("[cap] t+%2ds  video: %d AUs / %d bytes | audio: 0 frames / 0 bytes | kfr_req: 1 | "
      "jit: pkts=%d drops=0 nacks=0 resyncs=1 | tcp: dropAU=0 congest=0(q0/a0) nonref=0/0"
      % (s, au, byt, au * 2))
    if s % 5 == 0:
        p("[xbox] t=%d dec_fps=58.9 disp_fps=58.9 dec_ms=1.4 ring=0" % s)
    if kind == "crash" and s >= n:
        p("terminate called after throwing an instance of 'std::runtime_error'", err=True)
        p("  what():  sctp send: connection reset", err=True)
        sys.exit(134)
p("[cap] stopping...")
