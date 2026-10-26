# Stage 5 — v0.19.0: the Xbox plays xCloud with no PC

Status 2026-10-25: integrated, builds for Xenon (`variants/standalone.elf32`),
passes the offline end-to-end test on x86-64 and on big-endian ppc32 (qemu).
**No real xCloud session has been opened yet**: the first live test is yours.

`A` on a game = login refresh, `/play`, queue, `/sdp`, `/ice`, WebRTC
(ICE/DTLS-SRTP/SCTP), decoder, Opus audio and pads, all on the console.
`RT` held + `A` = the old path through the PC launcher/capture-rtp.

## Threads (hw thread = PIR)

| hw | who | what |
|---|---|---|
| 0 | main | menu, decode dispatch + GPU upload, `xn_play/xn_stop`, STATE lines -> `menu_model`, 2 s STAT |
| 1 | network | lwIP, `np_step` (HTTPS pipes, DNS, LOG), `xcw_net_step` (WebRTC UDP), `xn_net_step` -> `xcws_step` -> `xcw_poll` (ICE, DTLS, SRTP, jitter+NACK, RTCP RR/REMB, SCTP, datachannels, input), hook: USB pads at 1 kHz -> `xn_pad_frame`, Opus decode |
| 3 | session | `xs_task`: `xsm_step` (HTTPS), from PLAY until the answer is published; **v0.19.2**: then it hands the live phases to hw1 and exits (comes back only for re-login / full TLS handshake / re-signal / DELETE) |
| 4, 2, 5, 3 | decoder | libavcodec frame threads (**v0.19.2: 4 in a native game too**; hw3 is leased, see below) |

(v0.19.0/0.19.1 kept the session thread on hw3 for the whole game and gave
the decoder 3 threads: see "v0.19.2" at the end for the current design.)

Data paths:
- video: SRTP -> `xc_jitter` -> `deliver_au` (IDR gate, SPS/PPS cache) ->
  `stream_inject_au()` = `[u32 BE size][AU]` into the same 512 KB ring the TCP
  :9000 client filled -> `pull_next_au` -> decoder/GPU (unchanged). Ring full ->
  AU dropped + `xcw_resync_video` (wait IDR, keyframe request), = capture-rtp
  TcpOutServer. Main's IDR gating/backlog logic is unchanged; its KFRQ becomes
  `xn_request_keyframe` (PLI + `videoKeyframeRequested`, 1/s).
- audio: RTP header stripped -> `audio_rx_packet(seq, ts, opus)` (new source
  `AUDIO_SRC_RTC`, :9004 not opened) -> same jitter buffer/decoder.
- pads: the 24-byte frame + xCloud index (order of first use) ->
  `xcp_gamepad_from_wire24` -> `xcw_set_gamepad` per index. `xc_webrtc.c` now
  keeps 4 pads like capture-rtp `input_send_loop` v0.19: pad 0 always
  (heartbeat), a new index is announced with `gamepadChanged(i, true)` first,
  one shared sequence, on change / 50 ms heartbeat, 4 ms pacing, back-off.
- telemetry: `LOG ...` lines to the PC (UDP :9003, `/tmp/xbox-login.log`),
  never tokens. The 2 s `STAT` goes there as `LOG STAT ...` with WebRTC keys
  appended.

Lifetime rule (`source/native/xc_native.c`): a PLAY is accepted only when hw1
has detached from the previous session (PeerConnection destroyed, acked) and
the previous session thread has exited (its DELETE went out). Login (`Y`) and
session never overlap on hw3 (`Y` is refused while a session is busy, PLAY
while the login runs). `lt_clear_cancel()` before each session: a login
skipped with B used to leave `xcn_cancelled()` = 1, which cancels every HTTPS.

## Session thread decision: hw3 + decoder with 3 threads

Options weighed:
1. **Session on hw3, native decoder = 3 threads (hw 4,2,5)** — chosen.
2. Session on main between frames — rejected: one request = TLS handshake
   ~75 ms CPU + RTT, main blocked 100-200 ms every 5 s = 6-12 dropped frames
   per keepalive, and up to 20 s on a slow request (DELETE, re-login).
3. Longer keepalive only — does not remove the need for a thread (ICE,
   re-signaling, 401 re-login, DELETE); kept as a knob.

Numbers:
- Decode at the capped bitrate (b=AS 1500, ~0.8 Mbps measured) with 4 threads:
  `dec_fps` 57-61 = source rate, `dec_ms` (main blocked in avcodec) 1.4-2.3 ms,
  `ring=0`, `skipped=0` (`/tmp/xbox-stat.log`, 1500+ samples). The frame
  threads are far from saturated. Worst-case capacity with 3 threads using the
  pure-C single-thread figure (27.7 ms/frame, before AltiVec):
  3 / 27.7 ms = 108 fps, still > 60 fps with SMT contention on core 2. dt3
  only skipped at 3-5 Mbps (uncapped), per your earlier tests. A side effect: one
  frame less of frame-threading latency (~16 ms).
- Session load during the game: offline E2E, after STREAMING the session made
  exactly 1 request every 5 s (keepalive). ICE polling now stops at STREAMING
  (it used to run until end-of-candidates: 5 req/s x 75 ms = ~37% of hw3,
  the decoder's SMT sibling, if xCloud never sends it). So 75 ms / 5 s = 1.5%
  of hw3, and idle hw3 waits in `udelay` = `or 31,31,31` (low SMT priority).
- hw1: stage-4 bench on hardware = whole media path 4.1% of a thread at
  4 Mbps; DTLS longest blocking step 67 ms once per session. Now reported
  live as `hw1=` / `rtc_cpu=` in STAT.

Knobs (`make clean` after changing any):
`STANDALONE=1` (0 = v0.18.2 behaviour), `SESSION_HW=3`, `NATIVE_DEC_THREADS=3`,
`NATIVE_KBPS=1500`, `SESSION_KEEPALIVE_MS=5000`, `NATIVE_CONSENT_MS=0`.

## Build

    source scripts/env.sh
    deps/webrtc-xenon/build.sh xenon            # ~/webrtc-xenon/xenon (once)
    make -C app/h264play clean && make -C app/h264play VARIANT=standalone
    cp app/h264play/h264play.elf32 app/h264play/variants/standalone.elf32

One mbedTLS: `STANDALONE=1` links `libmbedtls-xcwebrtc.a` with
`xc360_mbedtls_webrtc_config.h` for everything (login too). `vmxaudit` = 0.

## Offline tests (no xCloud, fake tokens)

    app/h264play/hosttest/standalone/run_standalone.sh host 12   # ASan/UBSan
    XCW_LOSS=0.03 XCW_LOSS_ONCE=1 .../run_standalone.sh host 12  # NACK + RTT
    app/h264play/hosttest/standalone/run_standalone.sh ppc 12    # big-endian, qemu

`standalone_e2e.c` = the app's thread layout on the host: menu (`menu_model`)
on main, `xn_net_step` + pads + chat ping on an "hw1" thread, the session
thread from `xs_task_posix.c`, HTTPS = a fake GSSV (`port_rt.c`) whose
`/sdp` and `/ice` are bridged to a pion "xCloud" per session. Scenario:
PLAY game 1 -> STATE lines -> media -> 2 pads -> keyframe request ->
Back+Start (neutral pads, DELETE, teardown) -> PLAY game 2 + B in the queue
(DELETE, no PeerConnection) -> PLAY game 3 -> media -> Back+Start. 62 checks
plus `hosttest/webrtc/check_e2e.py` and `check_pads.py` on each streamed
session. It found a real bug in `xs_machine.c` (see THIRD_PARTY row): on a
real clock every attempt failed with "O video do xCloud nao chegou" right
after DTLS (unsigned underflow), invisible to the virtual-clock unit tests.

## First live test

1. Pendrive with `XC360/TOKEN.TXT` (the one the login already wrote). Copy
   `variants/standalone.elf32` (check `.sha256`) like the other variants.
2. On the PC: keep the LOG listener running (`/tmp/xbox-login.log`). The
   launcher/capture-rtp are NOT needed (leave them as they are, for RT+A).
3. Boot. Wait for "catalogo: Xbox (N jogos)". Pick a game, `A`.
4. The wait screen shows the session states (pt-BR, same as the launcher).
   `B` cancels (DELETE). In game, Back+Start 2 s = DELETE + menu.
5. Play a second game afterwards (teardown check).

LOG lines in order, and what it means if the log stops there:

| last line seen | meaning / next step |
|---|---|
| no `[xn] PLAY ...` | A went to the PC path: catalog not from the Xbox, or `[xn] xcw_global_init falhou` at boot |
| `[sess] fase IDLE -> AUTH`, then `ERROR Login ...` | refresh token: log in again (Y / login screen) |
| `[sess] ERRO: Falha ao iniciar jogo no xCloud: HTTP 4xx em play` | GSSV refused `/play` (UA, Game Pass, title); the body code is in the line |
| `fase START -> POLL`, `STATE QUEUE ...` for long | xCloud queue; `/state` every 1 s |
| `fase POLL -> WAIT_OFFER` then "WebRTC nao gerou a oferta" | hw1 could not create the PeerConnection/offer (`[xn]`/`[sdp]` lines) |
| `[xn] PeerConnection #1 ... offer feito`, `[sdp] video bandwidth cap: b=AS:1500` | offer OK |
| `fase SDP -> ICE_BRIDGE` / "Falha na negociacao SDP" | answer refused (body in the line) |
| `[ice] pre-SRD pronto: N remotos`, `[xcw] state -> checking` | ICE checks running |
| `[xcw] state -> failed : ICE: all candidate pairs failed` | no path to xCloud's candidates (Teredo/NAT) |
| `[xcw] state -> ice-connected`, then `dead media: ICE up but DTLS never completed` | DTLS refused: most likely the ECDSA cert; rebuild `deps/webrtc-xenon/build.sh xenon` with `ECDSA=0` |
| `[xcw] state -> dtls-connected`, `[dc] SCTP up`, no `HandshakeAck` | SCTP/datachannel problem |
| `[xcw] state -> streaming` but no `[xn] 1o AU no decoder` | no video RTP: `[xn] t=..` lines show `rtp=`, `srtp_err=` |
| `[xn] 1o AU ...`, `STATE STREAMING <jogo>` | video is in the decoder; then `LOG STAT ...` every 2 s |
| `[xcw] state -> closed : ICE consent ...` ~10 s into the game | rebuild with `NATIVE_CONSENT_MS=2500` |
| `[xn] STOP ...`, `STATE IDLE Sessao encerrada`, `thread da sessao terminou (DONE)` | clean stop, DELETE sent |

STAT keys (video/audio ones as before, then): `rtc=` (ok/ice/dtls/sctp/hsk/
failed/closed), `rtt_ms` (NACK -> retransmission; -1 = no loss yet),
`loss_pct`/`lost` (RTP loss left after NACK), `kbps` (video), `nack`, `kfrq`,
`pli`, `srtp_err`, `inj_drop` (decoder ring full), `jit_drop` (frames given
up), `pads` (pads known to xCloud), `in` (input reports), `hw1` (% of hw1
busy), `rtc_cpu` (% of hw1 inside WebRTC). Also `[tls] <host> ok: tcp Xms,
handshake Yms` per HTTPS request = the session thread's real TLS cost.

Fallback: `RT` + `A` = PC path (launcher + capture-rtp as before; the
TCP :9000 client is re-enabled until the next native PLAY). Or flash
`variants/v0182.elf32`, or build `STANDALONE=0`.

## Risks for the first live test

- ECDSA P-256 DTLS certificate (Chrome uses it; fallback `ECDSA=0`).
- GSSV accepting our `User-Agent: XCloud360/0.17` / no gzip on `/play` and
  `/sdp` (catalog works); TLS handshake per request on hw3.
- ICE consent: libpeer closes after 10 s without xCloud binding requests
  (capture-rtp lived without them) -> `NATIVE_CONSENT_MS=2500`.
- NIC RX ring (16 descriptors, polled every ~1 ms) during IDR bursts, and hw1
  blocked ~67 ms in the DTLS step / ~1.5 ms per Opus frame: NACK repairs
  single losses, a lost retransmission costs an IDR resync.
- Decoder with 3 threads if xCloud ignores the 1500 kbps cap in heavy scenes
  (watch `skipped`/`backlog`); `NATIVE_DEC_THREADS` cannot exceed the free hw
  threads (the session owns hw3).
- USB write of the rotated TOKEN.TXT from hw3 during a game (re-login on 401).
- B / Back+Start never interrupt a request in flight: the DELETE waits for
  it (up to ~20 s); PLAY says "Aguarde: encerrando a sessao anterior..."
  until then.

## v0.19.2 — session on hw1 during the game, decoder back to 4 threads

Hardware v0.19.x: `dec_fps` 32-36, `dec_ms` 20-22, `skipped` growing, with the
session thread parked on hw3 (SMT sibling of a decoder thread) and only 3
frame threads. v0.19.2 gives hw3 back to the decoder.

Design (simplest that keeps hw1 free of anything long):
1. **Keep-alive HTTPS** (`https_conn_*`, source/login/https.c): one TLS
   connection to the region host for the whole session (`/active`, `/play`,
   `/state`, `/connect`, `/sdp`, `/ice`, `/keepalive`, DELETE). Probed
   2026-10-25 (no tokens, `GET /v5/sessions/cloud/active` unauthenticated):
   `brs.core` keeps the connection (`Connection: keep-alive`, 150 requests
   on one connection, still alive after 31 s idle) but **never resumes a TLS
   session** (session ID and tickets: always "New"). Resumption (session ID,
   no config change) is still offered on every reconnect and logged.
   Handshakes are stepped one by one: `[tls] <host> ok: tcp X, handshake Y
   (cpu Z ms, completo|retomado)` — CPU excludes the network waits.
2. **Live phases on hw1, non-blocking** (`xsm_net_step`, from `xn_net_step`
   in the network-thread hook): at the answer publish (ICE_BRIDGE ->
   CONNECTING) the session thread sets owner = NET and exits. hw1 then does
   keepalive / ICE send / ICE poll with `https_conn_start/poll` (writes the
   TLS record, returns; reads it when bytes are there) and the cheap event
   transitions (ICE up, media -> STREAM_WAIT, video -> STREAMING). The live
   machine is looked at every 5 ms, a request in flight every 1 ms; a step
   that does nothing returns at once.
3. **Everything heavy goes back to a thread** (owner = WANT_THREAD, the same
   blocking code as before redoes it): 401 (re-login = 4 hosts + USB write),
   a reconnect the server will not resume (full handshake ~75 ms CPU —
   detected right after ServerHello, before any certificate/ECDHE work),
   a missing IP (DNS), re-signal, attempt failure, keepalive 404, and the
   final DELETE. hw1 re-spawns the session thread on hw3 when hw3 is free;
   the thread hands back as soon as the phase is live again.
4. **hw3 lease** (xc_native.c): hw3 is either the decoder's (4th frame
   thread, k_slots 4,2,5,3) or the session thread's, never both
   (`xenon_run_thread_task` is not atomic; pthread_create would fail).
   - game entry: the menu waits up to 2 s for the session thread to leave
     hw3, then `xn_dec_claim_hw3()` -> 4 threads (3 if it is still there);
   - in the game (every 10 ms, `xn_dec_threads_wanted`): the session asks
     for hw3 -> main reopens the decoder with 3 threads (waits for an IDR,
     forced keyframe request) and releases hw3; the session thread runs and
     hands back; after 3 s of STREAMING on hw1 main reopens with 4. Each
     switch costs one IDR (~1 RTT of frozen video); it only happens on a
     401 (token older than its lifetime), on a lost connection or a
     re-signal (video already frozen then);
   - Back+Start: `xn_stop` first, then avcodec_close releases hw3 and the
     DELETE runs on hw3 over the same keep-alive connection.
   B in the queue, play, SDP/ICE: unchanged (session thread, decoder closed).

Costs (offline E2E, keepalive 1 s, real mbedTLS client and server):
- request on the reused connection: 150-290 us CPU on x86-64 ASan / ppc32
  qemu (record crypto + HTTP + headers); full handshake 7-9 ms on x86 ASan,
  69-72 ms on ppc32 qemu. Hardware estimate (full handshake = 75 ms there):
  ~0.2-0.5 ms per keepalive, once every 5 s => `sess_cpu` well under 1 ms
  per 2 s, < 0.05 % of hw1. Largest single hw1 call: one https_conn_poll
  (decrypt + parse one response), no handshake crypto ever.
- before: one full handshake per request on hw3 (75 ms / 5 s = 1.5 % of
  the decoder's SMT sibling, plus `dec_ms` 20+).

STAT keys added at the end: `sess_cpu` (ms of hw1 driving the session per
2 s), `dec_thr` (decoder frame threads now), `sess` (net = hw1 drives, thr =
session thread on hw3, wait = waiting for hw3, done), `tls_req` (responses
on the keep-alive connection), `tls_hs` (full handshakes on it).

What to watch on the next hardware test (LOG):
- `[sess] fase CONNECTING: sessao entregue a thread de rede (hw1, #1)` right
  after `pre-SRD pronto`, then `[dec] jogo nativo: 4 threads de frame (hw
  4,2,5,3)`; `[menu] video do Xbox chegou ... entrando no jogo` without
  "(sessao ainda na hw3)".
- `[tls] brs...: pedido #2 / #20 na mesma conexao (cpu N us, sem handshake)`
  — N on the Xenon is the per-request cost; `tls_hs` stays at 1 the whole
  game and `tls_req` grows by 1 every 5 s.
- STAT: `dec_thr=4 sess=net`, `sess_cpu` < 1, `dec_ms` back near 2 ms,
  `dec_fps` ~60, `skipped` flat; `hw1` / `rtc_cpu` unchanged (~10 / ~2.5).
- if the server drops the connection: `[sess] hw1 devolve a sessao pra
  thread (servidor nao retomou a sessao TLS ...)`, `[dec] jogo nativo:
  threads 4 -> 3 (hw3 emprestada pra sessao)`, `[xn] thread da sessao de
  volta na hw3`, a full `[tls] ... completo` line, `entregue a thread de rede
  (#2)`, and ~3 s later `threads 3 -> 4 (hw3 de volta)`. Frequent ones would
  mean the GSSV closes keep-alive connections often (then a longer
  `SESSION_KEEPALIVE_MS` or a different design is needed).
- Back+Start: `[sess] hw1 devolve a sessao pra thread (stop ...)`, `[xn]
  thread da sessao de volta na hw3 (stop)`, `sessao encerrada (DELETE)`,
  `[tls] conexao brs...: N pedidos (N-1 reaproveitando o TLS), handshakes 1
  completos`.

Offline tests: `hosttest/standalone/run_standalone.sh host|ppc 12` now runs
the app's real https.c against a local TLS 1.2 fake GSSV (port_rt.c,
throw-away CA made by the script): keepalive every 1 s on one connection,
game 1 = forced 401 on a keepalive mid-stream (hw1 -> thread -> re-login on
the same region connection -> back to hw1, decoder 4 -> 3 -> 4), game 3 =
the server closes the keep-alive connection twice (with resumption: hw1
reconnects alone with an abbreviated handshake; without: one escalation and
a full handshake on the thread). `hosttest/session` has the same paths on
the virtual clock (`test_net_live`). Knob: `NATIVE_DEC_THREADS=3` = old
decoder layout (the session still leaves hw3; it just stays free).

## v0.19.3 — Back+Start keeps the session open in the menu

Back+Start (2 s) no longer ends the xCloud session. The game stays open in
the menu; picking it again is instant, picking another one ends it first.

| In the menu | native (xc_native) | PC bridge (launcher.py) |
|---|---|---|
| Back+Start | `xn_pause()`: no DELETE; keepalive/ICE/RTCP go on on hw1; the receiver drops complete AUs before `on_video_au` (`xcw_set_video_paused`, stats `paused_dropped`) and the Opus packets; NO keyframe requests (jitter loss and IDR-gate "starving" are muted); pads neutral (`xn_pad_neutral` on pause, `xn_pad_frame` ignored) | `PAUSE` on :9002 instead of `STOP`; the launcher keeps capture-rtp; the Xbox drops what arrives on :9000 (as the menu always did) |
| A on the same game | `xn_resume(id)`: no /play, no queue; IDR gate on + forced keyframe (PLI + videoKeyframeRequested); the first AU in the ring is an IDR | `PLAY` (launcher: same title = resume, just STATE); the new :9000 connection starts at an IDR |
| A on another game | `xn_stop()` (DELETE), the new `xn_play()` goes out when `xn_can_play()` (main.c `g_next_play`, B cancels) | `PLAY other` (the launcher stops the old capture = DELETE, then starts) |
| RT+A (via PC) with a native game open | `xn_stop()` then the PLAY to the PC | — |
| Reiniciar / Desligar | unchanged: `mm_power_run` -> `xn_busy()` -> `xn_stop()` | unchanged: STATE STREAMING -> STOP |
| xCloud ends it in the menu | `xsm_set_keep(1)`: an attempt failure (keepalive 404, dead video) becomes ERROR "Sessao xCloud encerrada pelo servidor" instead of a NEW session; `xn_session()` sees the finished machine and calls `xn_stop()` (WebRTC detached) -> `XN_SESS_NONE`; the menu shows the reason | the launcher does not restart a paused game: `STATE IDLE Sessao do xCloud encerrada: ...` -> the Xbox forgets it |

Menu query (for the menu layout: mark the game in progress):
`source/game_session.h`, `int xc_game_session(xc_game_session_t *out)`
(main thread, cheap, call every frame): `XC_GS_OPEN` + `id`/`name`/`via_pc`
= paused in the menu; also `XC_GS_STARTING/PLAYING/ENDING` for the native
session, `XC_GS_NONE` otherwise. Native layer: `xn_session()` /
`XN_SESS_*` in `native/xc_native.h`.

Tests: standalone phase 5 (s5..s7): play -> Back+Start (6 s in the menu:
keepalive +6, no DELETE, rtp flowing, ~30 AU/s dropped in the receiver,
nothing in the ring, no audio, 0 PLI/KFRQ) -> same game (first AU an IDR
~40 ms later, same session, PLI sent) -> Back+Start -> another game (DELETE
of the old one before its /play, GSSV event order) -> Back+Start -> fake
GSSV answers 404 to its keepalive -> ERROR, `XN_SESS_NONE`, no new session
by itself -> PLAY again = new session. Launcher: run_tests.sh scenarios
9-12 (resume without a new capture-rtp, replace, death/stall while paused
-> IDLE without restart, PAUSE while idle ignored).

Known limits: the offline test (X) with a game open lends hw3 to the
decoder, so a re-login/full TLS handshake of the paused session waits for
it to end; with the menu open the xCloud's own inactivity timeout still
applies (no input reaches the game).

## v0.20.0 — PC mode removed, new menu is the default

The app only works natively now. Removed from the app (code, build knobs and
PC-only host tests):

- the PC launcher control connection (TCP :9002, `ctrl_conn.c`), PLAY via the PC
  and RT+A, "B: usar o catálogo do PC" on the login screen and the launcher
  catalog fallback;
- video from the PC (TCP :9000 client in `tcp_stream.c`; the ring is now fed
  only by the WebRTC inject, one head update per AU), game audio from the PC
  (UDP :9004 in `audio_xenon.c`), pad frames and KFRQ to the PC (UDP :9001,
  `input_udp.c`);
- the text menu (`menu_ui.c`) and its model functions; build knobs `MENU`,
  `XUI`, `XBOX_LOGIN`, `STANDALONE` (all always on now);
- host tests `hosttest/ctrl_test.c` and the stale copies in `hosttest/login/`
  (`ctrl_test.c`, `menu_test.c`, `sim.c`, `run.sh`) plus `hosttest/mock_net/`.

v0.20.0 kept LOG/STAT over UDP :9003 to the PC listener. **v0.20.1 removed
it too**: `np_log_line` now feeds the beta-tester HTTPS upload
(`source/login/xc_logup.c`, see LOGS-UPLOAD.md; only with XC360/BETAKEY.TXT
on the USB drive). The STAT line ends with `ver=<build version>`.

`tools/xcloud-launcher` and `tools/xcloud-capture` stay in the repo (the app
no longer talks to them). The table above still lists the PC-bridge column for
history; it no longer applies to the app.

`game_session.h`: the `via_pc` field is gone (native sessions only).
