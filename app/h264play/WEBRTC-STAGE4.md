# Stage 4 — WebRTC on the Xbox (prepared, not yet integrated)

Status 2026-10-25: **the whole WebRTC receiver builds for Xenon and passes an
offline end-to-end test** (x86-64 and big-endian ppc32 under qemu) against a
local pion "xCloud". Not wired into `h264play` yet (main.c / Makefile /
tcp_stream.c belong to other agents right now): see "Integration steps".
No real xCloud session was started; nothing here has run on the console yet
except what the bench ELF will measure.

Replaces, on the Xbox, what `tools/xcloud-capture/rtp/capture-rtp.cpp` does on
the PC on top of libpeer: SDP offer + munging, ICE, DTLS-SRTP, video jitter
buffer + NACK, keyframe requests, RR + REMB, the four datachannels, the xCloud
message/control handshake and the gamepad input stream. Output = the same
things the app consumes today: Annex-B H.264 AUs, Opus packets.

## Files

| path | what |
|---|---|
| `deps/webrtc-xenon/build.sh [xenon\|host\|ppc\|all]` | builds libpeer + usrsctp + libsrtp + mbedTLS from `~/libpeer` (read-only) into `~/webrtc-xenon/<target>/{lib,include}` |
| `deps/webrtc-xenon/build-bench.sh [xenon\|host]` | bench ELF -> `variants/webrtc-bench.elf32` (+ full-API link check) |
| `deps/webrtc-xenon/patches/patch_libpeer.py`, `patch_usrsctp.py` | exact-anchor edits applied to the COPY in `~/webrtc-xenon/<t>/src` (abort if an anchor moved) |
| `deps/webrtc-xenon/mbedtls/xc360_mbedtls_webrtc_config.h`, `timing_alt.h` | mbedTLS config = login config + DTLS-SRTP (see "one mbedTLS") |
| `deps/webrtc-xenon/port/` | `xcw_port.h` (the only OS interface the libs use), `socket_xc.c` + `ports_xc.c` (replace libpeer socket.c/ports.c/mdns.c), `xcw_pthread_extra.c` (Xenon) |
| `deps/webrtc-xenon/shim/` | Xenon-only BSD headers (sys/socket.h, netinet/*, arpa/inet.h, net/if.h, ifaddrs.h, endian.h, ...) + `xcw_inet.c`; `-include xcw_shim.h` |
| `source/webrtc/xc_webrtc.h/.c` | **the C API** (single thread) |
| `source/webrtc/xc_webrtc_xsm.h/.c` | glue to the stage 3 session mailbox (`xsm_rtc_*`), call `xcws_step()` from `net_step()` |
| `source/webrtc/xc_webrtc_mt.h/.c` | generic cross-thread front end (if the stage 3 mailbox is not used) |
| `source/webrtc/xc_jitter.h/.c` | C port of the jitter buffer + NACK (fixed memory) |
| `source/webrtc/xc_proto.h/.c` | C port of green-nx `xcloud_protocol.cpp` (JSON + LE input reports) |
| `source/webrtc/xc_udp_xenon.h/.c` | `xcw_port.h` on the Xbox: lwIP UDP via lock-free rings to hw1, mftb clock, CTR-DRBG |
| `source/webrtc/bench/webrtc_bench.c`, `xcw_linktest.c` | CPU bench (Xbox + host), link check |
| `hosttest/webrtc/` | `run_all.sh`, `run_e2e.sh`, `e2e_recv.c`, `xc_udp_posix.c`, `peer_pion/` (Go), `check_e2e.py`, `proto_golden.sh`, `jitter_test.cpp`, `fake_xsm.c` |
| `variants/webrtc-bench.elf32` + `.sha256` | bench for XeLL |

## Build

    deps/webrtc-xenon/build.sh all          # host + ppc + xenon, ~1 min each
    deps/webrtc-xenon/build-bench.sh xenon  # variants/webrtc-bench.elf32
    app/h264play/hosttest/webrtc/run_all.sh # every offline check (~5 min)

Knobs: `ECDSA=0` (RSA-1024 DTLS cert like upstream libpeer; default ECDSA
P-256), `SRTP_CRYPTO=native` (libsrtp built-in AES/SHA1; default `mbedtls`
backend, 1.8x cheaper on the host), `LOG_LEVEL=3` (libpeer debug logs).
Outputs (`~/webrtc-xenon/xenon/lib`): `libpeer-xc.a`, `libusrsctp-xc.a`,
`libsrtp2-xc.a`, `libmbedtls-xcwebrtc.a`; headers in `.../include`
(`peer.h`, `peer_connection.h`, `xcw_port.h`, `srtp2/`, `mbedtls/`, configs);
`BUILDINFO.txt` records versions/flags. Nothing goes to `~/devkitxenon`.
Xenon flags = app MACHDEP with **`-mno-altivec`**; every library and the bench
ELF pass `vmxaudit.py` with 0 hits (the bench ELF has 0 opcode-4 words at all;
`source/webrtc/*.c` built with the app's `-maltivec` also audit 0).

## How the port works

- **libpeer** (`~/libpeer` @9319aa4 + green-nx + RTCP helpers) is used as is,
  plus `patch_libpeer.py`:
  - no `select()`: the UDP port is non-blocking; `xcw_poll()` paces the
    ICE/DTLS states at one `peer_connection_loop` per ms so libpeer's
    `AGENT_CONNCHECK_*` counters keep their meaning; STUN gathering waits with
    `ports_sleep_ms(1)` (= `xcw_idle`, which pumps lwIP on hw1);
  - **DTLS handshake is a step function** (upstream looped up to 15 s with
    `usleep`), retransmission timer kept (1/2/4 s) via `MBEDTLS_TIMING_ALT`;
  - SRTP failures are dropped and counted (upstream handed ciphertext to the
    media callbacks); cert validity ignored (fingerprint authenticates; the
    Xbox clock may be unset);
  - STUN PRIORITY written as 32-bit network order (upstream wrote the top
    half of a uint64 = 0 on big-endian);
  - ICE ufrag/pwd from a CSPRNG (upstream `srand(time(NULL))`);
  - a remote candidate added while CHECKING gets a pair (upstream ignored it);
  - `set_remote_description` copies SDP lines bounded (upstream: a line of
    256+ bytes overflowed a stack buffer — fatal on hw1);
  - an answered consent check refreshes the 10 s keepalive (only matters with
    `cfg.consent_interval_ms`, default off = capture-rtp behaviour);
  - usrsctp started with `usrsctp_init_nothreads`; `xcw_poll` drives its
    timers (`sctp_usrsctp_handle_timers`, every >= 10 ms).
- **usrsctp** 0.9.5 built AF_CONN-only (no INET/INET6, so no raw sockets and
  no recv threads at all), `SCTP_PROCESS_LEVEL_LOCKS`; its lower layer is
  libpeer's DTLS (`usrsctp_conninput` / conn output callback). Mutex/cond come
  from `deps/xenon-pthread` (already in the app); rwlocks, mutexattr,
  `pthread_self`, `nanosleep` are weak stubs in `xcw_pthread_extra.c`, valid
  because **the whole stack runs on one hw thread**. `read_random` ->
  `xcw_random`. Its clock (`SCTP_GETTIME_TIMEVAL`, ~50 call sites incl.
  per-packet RTT paths) is `xcw_now_us()` (mftb): libxenon's `gettimeofday()`
  is an SMC RTC round trip with ms resolution, shared with other SMC users.
  Nothing else in the stack calls gettimeofday/time on Xenon (`mbedtls_time`
  is the login's clock when set, else the "uninit" 0). xenon-gcc predefines `__linux__`, so usrsctp takes its Linux
  paths; the shim satisfies them. Shim `struct sockaddr` has the Linux layout
  (no `sa_len`), so the old `deps/patches/usrsctp-xenon.patch` sockaddr_conn
  hack is not needed (deps/build-libpeer-xenon.sh and deps/patches are
  superseded by deps/webrtc-xenon).
- **libsrtp** 2.4.2 with its mbedTLS crypto backend (AES-ICM via
  `MBEDTLS_CIPHER_MODE_CTR`, HMAC-SHA1), `WORDS_BIGENDIAN` on Xenon/ppc.
- **mbedTLS** 3.4.0: see below. Entropy = `mbedtls_hardware_poll` (the
  login's timebase-jitter source; a weak copy in `xc_udp_xenon.c` for builds
  without login), `xcw_random` = CTR-DRBG on top of it.
- **Self-signed DTLS cert = ECDSA P-256** (`CONFIG_DTLS_USE_ECDSA=1`).
  Upstream libpeer uses RSA-1024, whose keygen is 12-30 ms on the host
  (random, heavy tail) i.e. ~0.1-0.5 s+ on Xenon per session; ECDSA keygen +
  cert is 0.7 ms on the host. Browsers (Chrome) use ECDSA P-256 for WebRTC,
  so xCloud must accept it — **verify on the first live test** (fallback:
  `ECDSA=0`).

### One mbedTLS
The ELF can contain only one mbedTLS (same symbols; struct layouts depend on
the config). `xc360_mbedtls_webrtc_config.h` is the login config **verbatim**
plus DTLS/DTLS-SRTP/server/cookie/timing/x509-write/PK-write/GENPRIME/MD5/CTR.
At integration the app links `libmbedtls-xcwebrtc.a` INSTEAD of
`libmbedtls-xc360.a` and compiles everything (login too) with
`-DMBEDTLS_CONFIG_FILE="xc360_mbedtls_webrtc_config.h"`. Checked: all of
`source/login/*.c` compiles cleanly for Xenon with it. `deps/build-mbedtls-
xenon.sh` and its config are untouched.

## API (source/webrtc/xc_webrtc.h)

`xcw_global_init()` once; `xcw_create(&cfg)`; `xcw_create_offer()` (munged
offer, owned by x); `xcw_local_ufrag()`, `xcw_local_candidate(i)`;
`xcw_add_remote_candidate()` (best before the answer); `xcw_set_answer()`
(verbatim); `xcw_poll()` every <= 1 ms; `xcw_send(ch, data, n, text)`;
`xcw_get_stats()`; `xcw_destroy()`. `[any thread]`: `xcw_set_gamepad()`
(seqlock), `xcw_request_keyframe(force)`, `xcw_resync_video()`,
`xcw_set_video_kbps()`. Callbacks (on the WebRTC thread): `on_video_au(au, n,
info{rtp_ts, idr, nonref, params_prepended})`, `on_audio(seq, ts, opus, n)`,
`on_state`, `on_channel_msg`. States: new -> checking -> ice-connected ->
dtls-connected -> channels-open -> streaming, or failed (ICE failed / dead
media 12 s) / closed (consent timeout, SCTP closed).

capture-rtp behaviour kept, one by one:
- PeerConfiguration H264 + OPUS + BINARY datachannel; handlers registered
  before the offer; channels `control/controlV1/0`, `input/1.0/2`,
  `message/messageV1/4`, `chat/chatV1/6`, all reliable/ordered, created in
  `on_channel_open`; then `Handshake` on `message`.
- On `HandshakeAck`: authorizationRequest, gamepadChanged(0,true),
  userRequestedResolutionUpdate(alias), the 6 startup messages
  (w, h, kbps, fps), `client_metadata` from a FRESH serializer (seq 1 — the
  gamepad stream then also starts at seq 1, exactly like capture-rtp),
  videoKeyframeRequested + PLI. JSON/binary byte-identical to green-nx
  (`proto_golden.sh`, LE and BE).
- Input: on change but >= 4 ms apart, heartbeat 50 ms, rollback the sequence on
  a failed send, back-off 8/20/100 ms. `xcp_gamepad_from_wire24()` decodes the
  existing 24-byte Xbox wire format exactly like `input_udp_loop`.
- Video: jitter buffer identical to video_jitter.cpp (differential test:
  300/300 random traces, 250k packets, 28k AUs, 56k NACKs identical), NACK per
  gap, 100 ms hold, keyframe gating; `request_keyframe_throttled` (1/s unless
  forced); "starving" = consumer waiting for an IDR -> repeat 1/s; SPS/PPS
  cached and prepended to IDRs lacking them; optional drop of non-reference
  frames; SPS/PPS logged once.
- RR (from the jitter stats) + REMB(kbps) every second after the handshake.
- SDP: `a=setup:passive` -> `actpass` (NOTE: a no-op — this libpeer's offer
  says `a=setup:active`, which is what xCloud has been getting) and
  `b=AS`/`b=TIAS` after the m=video `c=` line. Answer applied verbatim.
- Dead media: ICE up and no DTLS after `dead_media_ms` (12000) -> FAILED.
  Session retry policy (2 attempts, 3 s cooldown) is the session layer's.
- Audio: RTP header stripped (CSRC/extension/padding), seq + ts + payload.

## Threads — recommendation: everything WebRTC on hw1

All UDP already lands on hw1 (lwIP owner), the media path costs a few % of a
thread (below), and hw0/hw2-5 are full (dispatch+GPU / decode). So:
`net_step()` (tcp_stream.c, hw1, every ~1 ms) calls `network_poll()` ->
`np_step()` -> `xcw_net_step()` -> `xcws_step()` (which runs `xcw_poll`).
AUs are pushed from the callback into the ring hw0 already reads; Opus goes to
the audio code that already runs on hw1. With the WebRTC thread == lwIP thread
`xc_udp_xenon.c` sends directly with `udp_sendto` (no ring hop) and receives
from an SPSC ring filled by the lwIP recv callback.
Blocking points on hw1, once per session: `xcw_create_offer` (ECDSA keygen +
cert, est. ~10 ms on Xenon; STUN wait up to ~1 s only if `stun_url` is set —
lwIP keeps being pumped through `xcw_net_set_pump`) and the DTLS client
crypto step (ECDHE + ECDSA verify/sign, est. ~35-50 ms; only handshake
traffic is in flight then). Fallback if the bench says hw1 is too tight: run
`xcw_poll` on hw3 with `DECODE_THREADS=3`; the port already supports a
WebRTC thread != lwIP thread (rings both ways, `xcw_net_step` flushes TX).
Do NOT drive it from two threads.

## Memory / stack

Per session (host-measured heap peak, same on ppc32): **~7.1 MB**: jitter pool
1.5 MB (1024 x 1.5 KB slots) + 2 MB AU assembly + 2 MB AU-with-params buffer
+ usrsctp ~230 KB + DTLS ~32 KB/context + agent/SDP ~60 KB. On the Xbox add
the UDP rings: 1.5 MB RX + 0.4 MB TX per socket (one socket). Stack
high-water of everything under `xcw_poll`: **14.9 KB** (x86-64) / **12.6 KB**
(ppc32); the hw1 stack (64 KB, 128 KB with audio) is enough — prefer 128 KB.
Static: ~512 KB entropy buffer only if the login is not linked.

## Offline E2E (no xCloud)

`hosttest/webrtc/run_e2e.sh <host|ppc> <loss> <seconds> [h264]` — pion v4
(`peer_pion/`, MIT, Go 1.23 in `~/webrtc-xenon/tools`) plays xCloud: answers
`a=setup:passive`, accepts our 4 channels, replies HandshakeAck, starts media
only after `clientdevicecapabilities`, streams a looped Annex-B file (STAP-A /
single NAL / FU-A) + 50 Opus/s, jumps to the next IDR on PLI or
videoKeyframeRequested, NACK responder on, validates every input report,
echoes chat ping/pong, counts RTCP. Signaling through files (offer, local
candidates, answer WITHOUT candidates, remote candidates trickled). The
receiver's UDP layer can drop video RTP before SRTP (`XCW_LOSS`,
`XCW_LOSS_ONCE`, `XCW_LOSS_AFTER`). `check_e2e.py` compares bytes.
Modes: direct API; `XCW_MT=1` (xc_webrtc_mt from a "session" thread + a
"network" thread); `XCW_XSM=1` (the stage 3 mailbox `xsm_rtc_*` + glue).

Results (12 s, GOLDEN-synth 720p, 30 fps; ppc = big-endian under qemu):

| run | AUs delivered / byte-identical | loss repaired by NACK | other |
|---|---|---|---|
| host, 0% | 360/360 identical, 0 corrupt | — | Opus 599/599 identical |
| host, 3% each seq once | 360/360 | 89/89 drops repaired | |
| host, 3% independent | 360/360 | 92 drops, 0 frames lost | earlier 5% run: 237 drops, 12 frames given up (double loss -> green-nx drop + IDR resync), 0 corrupt |
| ppc BE, 0% / 3% once / 3% | 357/357 each | 88-92 drops, 0 frames lost | |
| host 60 fps 20 s, 1% | 1199/1199 | 100/100 | |
| `--mt` host 3% once / ppc 2% | 360/360, 352/357 | all once-drops repaired | cross-thread wrapper OK |
| `--xsm` host 2% | 356/360 | 55/56 | events ice_up/media_up/video, no dead/dropped |

Every run: ICE + DTLS (ECDHE-ECDSA, use_srtp AES_CM_128_HMAC_SHA1_80) +
SCTP up in ~12-15 ms after the answer (host; ~100-125 ms under qemu), 0 SRTP
errors, channel set exactly control/controlV1/0 input/1.0/2 message/messageV1/4
chat/chatV1/6, 6 startup messages, all 4 control messages, input reports with
0 sequence gaps / 0 bad lengths (first gamepad seq 1), REMB 3000000 bps,
RR SSRC = video SSRC, chat round trip 48/48 at ~0.7 ms (host loopback).

## CPU numbers

Host (i5-3450 3.1 GHz) bench, same code as the Xbox bench:

| item | host | Xenon estimate (x10) |
|---|---|---|
| SRTP unprotect 1200 B (mbedTLS backend) | 8.9 us | ~90 us -> 37 ms/s at 4 Mbps = **3.7% of a thread** (x1.5 with a busy SMT sibling) |
| SRTP unprotect, libsrtp native crypto | 15.9 us | ~160 us -> 6.6% |
| jitter + FU-A depacketize | 0.5 us/pkt | ~5 us -> 0.2% |
| usrsctp 38-byte message, both stacks | 2.6 us | ~13 us per side -> <=0.3% at 250 msg/s |
| DTLS record 64 B seal+open | 1.2 us | ~12 us |
| DTLS handshake client CPU (ECDHE-ECDSA) | 3.4 ms | ~35-50 ms, one blocking step |
| ECDSA keygen + self-signed cert | 0.7 ms | ~7 ms |
| RSA-1024 keygen (NOT used) | 12-30 ms | 0.1-0.5 s+ |
| media path total at 4 Mbps (bench model) | 4.7 ms/s = 0.5% | **~5-7% of one hw thread** |
| whole E2E receiver process, 2.3 Mbps 60 fps | 1.7-1.9% of a core incl. its 0.5 ms poll loop | <= ~20% upper bound |

Scaling factor x10: the same single-thread C H.264 decode of the 720p golden
stream is 2.8 ms/frame on this host (ffmpeg, `-cpuflags 0`) vs 27.7 ms/frame
measured on the Xbox (ROADMAP-EXECUTION-P8-PLUS.md). Not included: lwIP RX and
NIC MMIO per packet (already paid today by the TCP stream on hw1) and the Opus
decode the app already does on hw1. Verdict: WebRTC fits on hw1; confirm with
the bench ELF.

### Bench ELF on hardware
`variants/webrtc-bench.elf32` (sha256 in `.sha256`). Copy it to the root of
the XeLL pendrive renamed **`xenon.elf`** (XeLL Reloaded autoboot list:
xell.conf, kboot.conf, xenon.elf, xenon.z...; see research/xell-reload-*.md)
or netboot it like the other variants, and boot XeLL (cold boot / eject, not
from Aurora: the bench starts hw1 for the SMT test, which needs a clean
`xenon_thread_startup`). No network or controller needed. ~10-30 s later the screen shows:
`[1]` SRTP protect/unprotect us/packet and % of a thread at 4 Mbps, sibling
idle AND with hw1 busy (the real hw1 case), plus the mbedTLS AES-CTR/HMAC
reference; `[2]` jitter us/packet; `[3]` DTLS: cert time, handshake wall,
client CPU, **longest client step** (= hw1 blocked), suite, 64 B record cost;
`[4]` usrsctp association + us/message; `[5]` ECDH/ECDSA/RSA-1024 primitives;
`[budget]` total ms/s and % of one hw thread. Photograph it. Decision rule:
budget < ~15% and longest DTLS step < ~100 ms -> hw1 as planned; SRTP far
above the estimate -> try `SRTP_CRYPTO=native` (compare with the reference
line); RSA line shows why ECDSA.

## Integration steps (for whoever owns main.c / Makefile / tcp_stream.c)

1. Makefile: knob `WEBRTC ?= 0`; with 1: `SOURCES += source/webrtc` (not the
   `bench/` subdir), `CFLAGS += -DXCLOUD360_WEBRTC=1 -I$(HOME)/webrtc-xenon/xenon/include`,
   switch the mbedTLS config to `-DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_webrtc_config.h\"`
   (replacing the login one), `LIBS = -L$(HOME)/webrtc-xenon/xenon/lib -lpeer-xc
   -lusrsctp-xc -lsrtp2-xc -lmbedtls-xcwebrtc ...` instead of `-lmbedtls-xc360`
   (inside `--start-group/--end-group` or after the objects), and always
   compile `deps/xenon-pthread` (usrsctp needs pthread mutex/cond even with
   DECODE_THREADS=1). Re-run `hosttest/login/run_login.sh unit`.
2. Boot (main, before `stream_start_thread`, like `np_init`):
   `xcw_global_init()`, `xcw_set_log_hook(xc_log-like)`, `xcw_net_set_pump(net_step)`
   (or a function doing `network_poll(); np_step(); xcw_net_step();`),
   `xcw_set_resolver(xcn_resolve)` only if a STUN hostname is used.
3. `net_step()` in tcp_stream.c, after `np_step()`: `xcw_net_step();
   xcws_step(&g_xcws);` (`xcws_init(&g_xcws, &session_machine, &cfg)` once).
   Keep NET_POLL_PAUSE_US at 1000.
4. Video: `cfg.on_video_au` writes `[u32 BE size][AU]` into the same ring
   `stream_read()` serves (needs a small `stream_inject()` in tcp_stream.c,
   since `ring_write` is static). If the ring lacks room: drop the AU and call
   `xcw_resync_video()` (= TcpOutServer congestion: wait IDR + keyframe
   request). The decoder's KFRQ becomes `xcw_request_keyframe(x, 0)`; a fresh
   consumer (menu -> game) is `xcw_request_keyframe(x, 1)` + resync.
5. Audio: `cfg.on_audio(seq, ts, opus, n)` -> build the same 6-byte header
   `[seq u16 BE][ts u32 BE]` + payload the UDP :9004 path receives and hand it
   to the audio module (needs an `audio_net_feed()`-style entry there).
6. Input: where main.c packs the 24-byte wire for `input_udp_send`, call
   `xcp_gamepad_from_wire24(buf, 24, &pad); xcw_set_gamepad(xcws_pc(&g), &pad);`
   (thread-safe, but guard against the PC being replaced: e.g. read
   `xcws_pc` once per frame under the app's session state).
7. cfg: `video_kbps` = what the launcher used (**1500**, `--kbps 1500`; the
   capture-rtp default is 3000), 1280x720@30, alias "720", `stun_url` NULL or
   an IP literal (capture-rtp used stun.l.google.com:19302 — resolve it on the
   session thread first if wanted), `drop_nonref` as `--drop-nonref`.
8. THIRD_PARTY.md: add pion (test only), usrsctp/libsrtp/mbedTLS/libpeer as
   linked into the ELF, and the GPL note below.

## Risks / unknowns for the first live xCloud test

- **ECDSA certificate** accepted by xCloud's DTLS server? (Chrome uses ECDSA,
  so very likely; else `ECDSA=0`.)
- **ICE**: libpeer controlling with tie-breaker 0, STUN PRIORITY now a real
  value (was 0 on BE). xCloud's Teredo-encoded candidates must be decoded by
  the session layer (green-nx `session.cpp`; stage 3 does it). AGENT_MAX_
  CANDIDATES = 10 remote.
- **Consent**: libpeer closes after 10 s without a binding REQUEST from the
  peer; xCloud sends them (capture-rtp lives long). If sessions die at ~10 s,
  set `consent_interval_ms = 2500`.
- **NIC RX ring = 16 descriptors** polled every 1 ms; IDR bursts above
  ~16 packets/ms, or hw1 blocked (DTLS step, Opus decode, big drains) overflow
  it. NACK repairs single losses (shown above); a lost retransmission costs a
  frame + IDR resync (green-nx design). `max_drain` (32) bounds work per poll.
- **SRTP replay window 128** (libsrtp default): a retransmission older than
  128 packets is rejected (counted in `srtp_errors`). Fine at 3 Mbps / 30 ms
  RTT; raise `window_size` if seen.
- **First packet of the stream lost** cannot be NACKed (no earlier seq): ~1 s
  to first picture (keyframe request throttle). Same as capture-rtp.
- **DTLS crypto step blocks hw1 ~35-50 ms** once per session (estimate).
- **Ciphersuite**: GCM/ChaChaPoly only (login config has no CBC). pion picked
  ChaCha20-Poly1305 with mbedTLS' order; xCloud will pick.
- **Data channels before DCEP ACK**: as libpeer/capture-rtp, sends right after
  DCEP OPEN; xCloud accepted it with capture-rtp.
- Real-hardware-only: lwIP pbuf/udp behaviour under load, newlib malloc from
  hw1 concurrently with the decoder threads (the app already mallocs from
  several threads), mftb clock math, the weak/strong `mbedtls_hardware_poll`.
- libpeer does not deregister the usrsctp address on destroy (tiny leak per
  PeerConnection; one per attempt).
- capture-rtp quirk not reproduced: it sent libpeer's `onicecandidate` value
  (the whole offer SDP) as one "candidate" to `/ice`; through the stage 3
  mailbox that would become "v=0". We send one `candidate:` line per local
  candidate (green-nx engine.cpp behaviour); the offer also carries them.

## Licenses

libpeer MIT, usrsctp BSD-3, libsrtp BSD-3, mbedTLS Apache-2.0, pion (test peer
only, not shipped) MIT. **GPL-3.0**: `source/webrtc/xc_proto.c` is a close
port of green-nx `src/core/xcloud_protocol.cpp` and `source/webrtc/xc_jitter.c`
a close port of green-nx's VideoJitterBuffer (via video_jitter.cpp); green-nx
is GPL-3.0, so an ELF linking them is GPL-3.0 — same situation as capture-rtp
today (see its header). The rest of `source/webrtc` is ours.
