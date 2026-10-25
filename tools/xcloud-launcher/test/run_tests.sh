#!/bin/bash
# End-to-end test of launcher.py with fake capture-rtp / greennx / Xbox.
# Uses ports 19002 (control) / 19000 (video) and only files under $T, so a
# real session on 9000/9001/9002 and /tmp/capture-rtp.log is never touched.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
T="${T:-/tmp/xcl-launcher-test}"
CPORT=19002
VPORT=19000
rm -rf "$T"; mkdir -p "$T/ctl"
export FAKE_CAP_DIR="$T/ctl" FAKE_AUTH_FILE="$T/ctl/authed" FAKE_LOGIN_DELAY=3

python3 -u "$HERE/../launcher.py" --port $CPORT --video-port $VPORT \
    --repo "$REPO" --capture-bin "$HERE/fake_capture.py" --greennx "$HERE/fake_greennx.py" \
    --state-file "$T/launcher.json" --catalog-cache "$T/catalog.json" \
    --log-dir "$T/logs" --compat-log "$T/capture-rtp.log" \
    --stall-secs 5 --grace-secs 8 --backoff 1,2,3 --stop-timeout 3 \
    > "$T/launcher.out" 2>&1 &
LPID=$!
trap 'kill -INT $LPID 2>/dev/null; sleep 2; kill -9 $LPID 2>/dev/null' EXIT
sleep 1

X() { python3 -u "$HERE/fake_xbox.py" --port $CPORT "$@"; local rc=$?; [ $rc = 0 ] || FAIL="$FAIL $CUR"; return $rc; }
FAIL=""
banner() { CUR="$1"; echo; echo "==================== $1 ===================="; }

banner "1 LOGIN (no token, no cache) + LIST"
X '>HELLO|1' '?IDLE:5' '>LIST' '?CATALOG:5' '?LOGIN:10' '?=LOGIN:10' '?CATALOG:20' '?=END:20'

banner "2 PLAY with queue -> QUEUE -> STARTING -> STREAMING, PING, STOP"
echo "queue:3" > "$T/ctl/ANNO1800.modes"
X '>HELLO|1' '?IDLE:5' '>PLAY|ANNO1800' '?STARTING:5' '?QUEUE:10' '?STARTING:10' '?STREAMING:20' \
  '>PING' '?=PONG:3' '>STOP' '?IDLE:10'

banner "3 recents ordering (ANNO1800 then FORZAHORIZON5 played -> FORZA, ANNO first)"
X '>HELLO|1' '>PLAY|FORZAHORIZON5' '?STREAMING:20' '>STOP' '?IDLE:10' '>LIST' '?=END:10'
echo "state file:"; cat "$T/launcher.json"; echo

banner "4 stall -> RECONNECTING -> STREAMING"
printf 'stall:3\nok\n' > "$T/ctl/STALLGAME.modes"
X '>HELLO|1' '>PLAY|STALLGAME' '?STREAMING:20' '?RECONNECTING:20' '?STREAMING:40' '>STOP' '?IDLE:10'

banner "5 crash loop -> ERROR after 3 restarts"
printf 'crash:2\ncrash:2\ncrash:2\ncrash:2\ncrash:2\n' > "$T/ctl/CRASHGAME.modes"
X '>HELLO|1' '>PLAY|CRASHGAME' '?STREAMING:20' '?RECONNECTING:20' '?RECONNECTING:30' '?RECONNECTING:30' '?ERROR:40' '~3'
echo "CRASHGAME runs: $(grep -c 'title=CRASHGAME' "$T/ctl/runs.log") (expect 4 = 1 + 3 restarts)"

banner "6a client drops while streaming, reconnects within grace -> still STREAMING"
X '>HELLO|1' '>PLAY|HALOINFINITE' '?STREAMING:20'
sleep 3
X '>HELLO|1' '?STREAMING:3' '~1'
banner "6b new connection replaces the old one"
python3 -u "$HERE/fake_xbox.py" --port $CPORT '>HELLO|1' '~6' &
OLD=$!
sleep 1.5
X '>HELLO|1' '?STREAMING:3' '>PING' '?=PONG:3'
wait $OLD
banner "6c client gone longer than grace (8s) -> game stopped"
sleep 11
X '>HELLO|1' '?IDLE:3'

banner "7 capture-rtp credential failure -> login -> resume PLAY"
rm -f "$FAKE_AUTH_FILE"
X '>HELLO|1' '>PLAY|ASSASSINSCREEDODYSSEY' '?LOGIN:15' '?=LOGIN:10' '?STARTING:20' '?STREAMING:30' '>STOP' '?IDLE:10'

banner "8 capture ignores SIGINT -> SIGKILL after stop-timeout; PLAY replaces running game"
echo "hang" > "$T/ctl/ZETAGAME.modes"
X '>HELLO|1' '>PLAY|ZETAGAME' '?STREAMING:20' '>PLAY|NONAMEGAME' '?STARTING:5' '?STREAMING:30' '>STOP' '?IDLE:10'

banner "launcher log"
cat "$T/launcher.out"
echo
echo "fake capture runs:"; cat "$T/ctl/runs.log"
echo "session logs:"; ls "$T/logs"
echo
[ -z "$FAIL" ] && echo "ALL SCENARIOS PASSED" || echo "FAILED:$FAIL"
