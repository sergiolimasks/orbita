#!/usr/bin/env python3
"""Fake Xbox control client speaking the launcher protocol.

  fake_xbox.py [--host H] [--port P] STEP...

Steps:
  >CMD|arg        send a line ('|' becomes TAB), e.g. '>PLAY|ANNO1800'
  ?CODE[:secs]    wait for 'STATE<TAB>CODE...' (default 30 s), fail on timeout
  ?=WORD[:secs]   wait for any line whose first field is WORD (END, LOGIN, PONG)
  !CODE:secs      read for secs, FAIL if 'STATE<TAB>CODE' arrives (v0.19.3)
  ~secs           just read/print for that long
Every received line is printed with a timestamp (TAB shown as ' | ').
Exit code 0 when all waits succeed, 1 otherwise.
"""
import argparse
import socket
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=19002)
ap.add_argument("steps", nargs="+")
a = ap.parse_args()

t0 = time.time()
s = socket.create_connection((a.host, a.port), timeout=5)
s.settimeout(0.2)
buf = b""


def out(prefix, line):
    print("[xbox %6.1fs] %s %s" % (time.time() - t0, prefix, line.replace("\t", " | ")), flush=True)


def read_lines(deadline):
    global buf
    while time.time() < deadline:
        if b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode("utf-8")
            continue
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            out("!!", "connection closed by launcher")
            return
        buf += d


ok = True
games = 0
for step in a.steps:
    if step.startswith(">"):
        line = step[1:].replace("|", "\t")
        out(">>", line)
        s.sendall((line + "\n").encode("utf-8"))
    elif step.startswith("!"):
        spec, _, secs = step[1:].partition(":")
        for line in read_lines(time.time() + float(secs or 3)):
            out("<<", line)
            f = line.split("\t")
            if f[0] == "STATE" and len(f) > 1 and f[1] == spec:
                out("!!", "unexpected STATE %s" % spec)
                ok = False
        if not ok:
            break
    elif step.startswith("~"):
        for line in read_lines(time.time() + float(step[1:])):
            out("<<", line)
    elif step.startswith("?"):
        spec, _, secs = step[1:].partition(":")
        secs = float(secs or 30)
        found = False
        for line in read_lines(time.time() + secs):
            f = line.split("\t")
            if f[0] == "GAME":
                games += 1
                if games > 12:
                    continue
            out("<<", line)
            if spec.startswith("="):
                if f[0] == spec[1:]:
                    found = True
                    break
            elif f[0] == "STATE" and len(f) > 1 and f[1] == spec:
                found = True
                break
        if not found:
            out("!!", "TIMEOUT waiting for %s" % spec)
            ok = False
            break
        if spec == "=END" and games > 12:
            out("..", "(%d GAME lines total, first 12 shown)" % games)
s.close()
out("--", "done ok=%s" % ok)
sys.exit(0 if ok else 1)
