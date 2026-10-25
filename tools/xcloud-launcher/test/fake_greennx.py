#!/usr/bin/env python3
"""Fake greennx CLI (games / login) for launcher tests.

$FAKE_AUTH_FILE   present = signed in. `login` creates it; `games` fails like
                  the real CLI when it is missing.
$FAKE_LOGIN_DELAY seconds the "user" takes to enter the code (default 3).
$FAKE_LOGIN_FAIL  if set, the code expires instead.
"""
import os
import sys
import time

auth = os.environ.get("FAKE_AUTH_FILE", "/tmp/fake-gnx-auth")
cmd = sys.argv[1] if len(sys.argv) > 1 else ""

GAMES = [
    ("ASSASSINSCREEDODYSSEY", "Assassin's Creed Odyssey"),
    ("ANNO1800", "Anno 1800™"),
    ("AGAMEABOUTDIGGINGAHOLE", "A Game About Digging A Hole™"),
    ("POKEMONLIKE", "Pokémon® Café Édition\tSpecial"),
    ("FORZAHORIZON5", "Forza Horizon 5"),
    ("ZETAGAME", "zeta game (lowercase)"),
    ("LONGNAME", "The Extraordinarily Long Title Of A Game That Goes On And On Forever Edition"),
    ("NONAMEGAME", ""),
    ("STALLGAME", "Stall Game"),
    ("CRASHGAME", "Crash Game"),
    ("HALOINFINITE", "Halo Infinite"),
]

if cmd == "games":
    print("Fetching streaming credentials...", flush=True)
    time.sleep(1)
    if not os.path.exists(auth):
        sys.stderr.write("error: not signed in — run login first\n")
        sys.exit(1)
    print("Cloud endpoint: https://brs.core.gssv-play-prod.xboxlive.com")
    print("Catalog: 2649 titles, %d playable\n" % len(GAMES))
    print("Resolving names from the Store catalog...\n")
    for tid, name in GAMES:
        if name:
            print("  %-40s  %s" % (tid, name))
        else:
            print("  %s" % tid)
    sys.exit(0)
if cmd == "login":
    sys.stdout.write("\nTo sign in, open https://www.microsoft.com/link\nand enter code: AB12CD34\n\nWaiting")
    sys.stdout.flush()
    for _ in range(int(float(os.environ.get("FAKE_LOGIN_DELAY", "3")))):
        time.sleep(1)
        sys.stdout.write(".")
        sys.stdout.flush()
    if os.environ.get("FAKE_LOGIN_FAIL"):
        print("\nCode expired, run login again.")
        sys.exit(1)
    open(auth, "w").write("{}")
    print("\nSigned in!")
    sys.exit(0)
print("usage: greennx <login|logout|whoami|games|ids|search <QUERY>|stream-test <TITLEID> [--ads]>")
sys.exit(1)
