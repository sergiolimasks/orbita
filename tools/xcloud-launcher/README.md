# xcloud-launcher

PC-side daemon the Xbox app talks to so it can pick a game: it serves the
xCloud catalog, starts/stops `capture-rtp` for the chosen title, reports
queue/provisioning/streaming status, restarts dead sessions on its own and
drives the Microsoft device-code login when the saved token is missing or
expired. Python 3 stdlib only.

```
Xbox ──TCP :9002 (control, this daemon)──► launcher.py ──spawns──► capture-rtp <TITLE> --tcp-out 9000 ...
Xbox ◄─TCP :9000 (video, unchanged)──────────────────────────────── capture-rtp
                                           launcher.py ──spawns──► greennx games | greennx login
```

## Running it

```bash
cd /mnt/storage/projetos/PROJETOS/XCloud360
python3 -u tools/xcloud-launcher/launcher.py 2>&1 | tee -a /tmp/xcloud-launcher.log
# or in the background:
nohup python3 -u tools/xcloud-launcher/launcher.py >> /tmp/xcloud-launcher.log 2>&1 &
```

The defaults are the real paths (repo = two levels above the script, port 9002,
video port 9000, `~/green-nx/build/pc/greennx`, `tools/xcloud-capture/rtp/capture-rtp`).
Do not start it alongside a `capture-rtp` you launched by hand: the launcher
refuses to start a game while something else is listening on the video port
(you get `STATE ERROR "Porta de video 9000 ocupada no PC"`). Stopping the
launcher (Ctrl-C / SIGTERM) also stops its capture-rtp.

| option | default | |
|---|---|---|
| `--port` | 9002 | control port for the Xbox (`--bind` 0.0.0.0) |
| `--video-port` | 9000 | passed to capture-rtp `--tcp-out` |
| `--repo` | repo root | capture-rtp cwd (it writes `tools/xcloud-capture/output/...`) |
| `--capture-bin` | `<repo>/tools/xcloud-capture/rtp/capture-rtp` | |
| `--greennx` | `~/green-nx/build/pc/greennx` | |
| `--state-file` | `~/.config/xcloud360/launcher.json` | recents (last 10) + last title |
| `--catalog-cache` | `~/.config/xcloud360/catalog.json` | output of `greennx games`, parsed |
| `--log-dir` | `/tmp/xcloud-launcher` | one `capture-<TITLE>-<ts>.log` per session |
| `--compat-log` | `/tmp/capture-rtp.log` | symlink to the latest session log (`''` = off) |
| `--seconds` / `--kbps` | 21600 / 1500 | capture-rtp args; `--extra-arg=--drop-nonref` (repeatable) adds more |
| `--stall-secs` | 20 | video AU counter frozen this long after CONNECTED → restart |
| `--silence-secs` | 180 | capture-rtp prints nothing this long *before* CONNECTED → restart |
| `--max-restarts` / `--restart-window` | 3 / 600 | at most 3 automatic restarts per 10 min, then ERROR |
| `--backoff` | `3,10,30` | delay before restart 1, 2, 3 |
| `--grace-secs` | 300 | how long a game survives the Xbox control connection dropping |
| `--stop-timeout` | 15 | SIGINT → wait → SIGKILL |
| `--catalog-max-age` | 21600 | cache older than this is refreshed in background on LIST |

## Protocol (TCP, UTF-8 lines ending in `\n`, fields separated by TAB)

Xbox → PC: `HELLO\t<ver>`, `LIST`, `PLAY\t<TITLEID>`, `STOP`, `PING`.

PC → Xbox: `STATE\t<CODE>\t<text>`, `LOGIN\t<url>\t<code>`,
`GAME\t<TITLEID>\t<name>\t<recent 0|1>` … `END`, `PONG`.

CODE ∈ IDLE, LOGIN, CATALOG, STARTING, QUEUE, STREAMING, RECONNECTING, ERROR.
Texts are short Portuguese, ASCII only (≤ 80 chars), e.g.
`Na fila: posicao 12, ~3 min`, `Reconectando (1/3): Video parou (20s sem quadros)`.
In STREAMING the text is the game name. Game names are NFKD-folded to ASCII
(™®© dropped, tabs/newlines removed, ≤ 60 chars).

One Xbox at a time: a new connection closes the previous one. STATE is only
sent on HELLO and on changes; LIST is answered with the GAME…END block.

## Behaviour

* **HELLO** → current `STATE` (+ `LOGIN url code` if a login is pending).
* **LIST** → served from the cache file immediately; refreshed in the
  background if older than 6 h or the last fetch failed. With no cache:
  `STATE CATALOG`, run `greennx games`, then the previous state + the list.
  Order: recently played (last 10, `recent=1`, most recent first), then the
  rest alphabetically by name.
* **PLAY** → title goes to the recents, any running capture is stopped
  (SIGINT, 15 s, SIGKILL), capture-rtp is started. States follow its log:
  `[cap] fetching streaming credentials` → STARTING; `[XCLOUD360-STATE]`
  `WaitingForResources` → QUEUE (position/ETA taken from any
  `*position*` / `estimatedTotalWaitTimeInSeconds` / `*wait*seconds*` field
  in that JSON); Provisioning/Provisioned → STARTING; `[cap] CONNECTED` +
  the `[cap] t+Ns video: N AUs` counter growing → STREAMING.
  PLAY of the title that is already starting/streaming just re-sends STATE.
* **Watchdog** → capture-rtp exiting on its own, or the AU counter not growing
  for 20 s after CONNECTED (the silent "sctp not connected" death), restarts
  the same title (STATE RECONNECTING; backoff 3/10/30 s). A 4th failure inside
  10 minutes → `STATE ERROR` with the last reason (from `session failed:`,
  `start_cloud failed:`, `what():`, `all N attempts failed`, ...).
* **Login** → `greennx games` or capture-rtp failing with *not signed in /
  no saved login / saved login expired / sign in again / invalid_grant* (or
  an HTTP 401 before `[cap] endpoint host`) runs `greennx login`; URL and code
  go to the Xbox (`STATE LOGIN` + `LOGIN`) and to the launcher log; after
  `Signed in!` the pending LIST/PLAY is resumed. If capture-rtp still fails on
  credentials right after that login → ERROR (no login loop).
* **STOP** → stops capture (and a pending login), `STATE IDLE`.
* **Xbox disconnects** while a game runs → the game keeps running for 5 min;
  on reconnect HELLO gets `STATE STREAMING <name>`. After 5 min it is stopped.
* **PING** → `PONG`.

Logging: everything timestamped on stdout. capture-rtp's full output (stdout +
stderr, libpeer DEBUG included) goes straight to the per-session file; the
launcher echoes the interesting lines, one `[cap] t+` / `[xbox]` line every
30 s, and a running count of `sctp not connected` errors.

`/tmp/capture-rtp.log` is replaced by a symlink to the newest session log,
except while a capture-rtp the launcher did not start is running (then it is
left alone and a line is logged). A previous regular file there is moved to
`/tmp/capture-rtp.log.old`.

## Tests

`test/run_tests.sh` runs the launcher on ports 19002/19000 with
`test/fake_capture.py` (scripted modes: queue, stall, crash, failstart,
authfail, hang), `test/fake_greennx.py` (games/login) and `test/fake_xbox.py`
(protocol client with wait-for-state steps). All files go to
`/tmp/xcl-launcher-test`; nothing real is touched.

```bash
bash tools/xcloud-launcher/test/run_tests.sh      # ~2 min, ends with ALL SCENARIOS PASSED
python3 tools/xcloud-launcher/test/fake_xbox.py --port 9002 '>HELLO|1' '>LIST' '?=END:120'   # poke a real launcher
```

## Known limitations

* Queue info: the real `/state` JSON for `WaitingForResources` has never been
  captured here, so the field names are guessed (any key with `position`,
  `estimatedTotalWaitTimeInSeconds`, any `*wait*second*`). With none present
  the text is `Na fila do xCloud...`.
* Restarting capture-rtp closes the video TCP connection on :9000; the Xbox
  must reconnect to the video port by itself (it gets RECONNECTING/STREAMING
  on the control channel as a hint).
* capture-rtp itself waits up to 30 min per attempt in the xCloud queue; the
  launcher does not impose a separate start timeout (only the 180 s silence
  rule before CONNECTED).
* A capture-rtp started by hand on the same video port blocks PLAY; the
  launcher never kills processes it did not start.
* No authentication on the control port (LAN only).
