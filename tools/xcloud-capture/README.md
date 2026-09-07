# xcloud-capture

PC-side tool that authenticates an Xbox / Microsoft account, creates a real xCloud streaming session, and captures the RTP H.264 bitstream + full protocol trace so the XCloud360 project has a **GOLDEN STREAM** (spec §16/§17) to develop the on-console decoder against.

**Do not attempt to record commercial gameplay footage with this tool.** It exists purely to characterize the bitstream (profile, level, slices per frame, GOP, IDR interval, bitrate) so the XCloud360 decoder can be built against real xCloud data instead of guessed parameters.

## Stack

- **Green-NX** (`rmrf404/green-nx`, GPL-3) — provides a battle-tested C++ CLI (`greennx`) that handles Microsoft device-code auth, GSSV session creation, SDP offer/answer, and ICE candidate exchange. Used here as an **external subprocess** — no source is vendored into XCloud360, GPL boundary preserved.
- **libpeer** (`sepfy/libpeer`, MIT) pin `9319aa434cb9e893faed0293ba9d2a21eca59c8b` + mbedTLS 3.4.0 + libsrtp + usrsctp — the same WebRTC stack XCloud360 will use on the console. Built PC-side for the RTP capture stage.
- **libavcodec / ffmpeg** — for H.264 bitstream parsing (SPS/PPS/slice headers). Runtime only, not for decode.

## Ordered milestones

1. **5.1 — bootstrap** *(this file, `setup.sh`)*. Clone/build Green-NX PC, verify libcurl deps, confirm `greennx` binary runs.
2. **5.2 — auth + catalog snapshot**. `login.sh` runs device-code flow; `games.sh` dumps the account's playable library. Confirms the account can reach xCloud before we spend time on streaming.
3. **5.3 — signaling capture**. `capture-signaling.sh` runs `greennx stream-test` against a chosen title, tees SDP offer/answer + ICE candidates + endpoint metadata to `output/<title>-<ts>/session.sdp` + `signaling.json`. **Already yields ~40% of §16's required analysis (profile-level-id offered, ICE mechanism, offering used) without needing any WebRTC code in this project.**
4. **5.4 — RTP capture**. Adapt libpeer + engine glue from Green-NX Switch reference (`src/switch/stream/engine.cpp`) to POSIX sockets. Bring up peer connection with the SDP obtained in 5.3, receive RTP video packets on the correct SSRC, depacketize (RFC 6184: STAP-A / FU-A / single-NAL), reconstitute Annex-B `.h264`.
5. **5.5 — bitstream analysis**. Parse SPS/PPS/slice headers. Emit `analysis.json` + `analysis.txt` per spec §16 requirements:
   - `profile_idc`, constraint flags, `level_idc`
   - width, height
   - `entropy_coding_mode` (CAVLC/CABAC)
   - `num_ref_frames`, `pic_order_cnt_type`, `frame_mbs_only`
   - slices per frame, `first_mb_in_slice`
   - NAL type distribution
   - IDR interval
   - bitrate median + p95 + max
   - measured fps

## Layout

```
tools/xcloud-capture/
├── README.md              (this file)
├── setup.sh               (bootstrap Green-NX PC + deps)
├── login.sh               (device-code sign-in)
├── games.sh               (dump playable library)
├── capture-signaling.sh   (5.3 — SDP + ICE trace)
├── capture-rtp/           (5.4 — libpeer + engine glue, later)
├── analyze/               (5.5 — bitstream parser, later)
└── output/                (per-title captures, gitignored)
```

## Where does the token go?

`greennx login` stores refresh + access tokens in `$HOME/.green-nx-tokens.json` (hardcoded in `src/cli/main.cpp::token_store_path()`). We inherit that path — do NOT copy it into the XCloud360 repo, do NOT log its contents.

## Known blocker: ad-supported queue time

The `xgpuwebf2p` (ad-supported) offering that non-Game-Pass accounts fall into is heavily rate-limited by Microsoft. Observed 2026-09-07 (Brazilian holiday): server reported **~205 min queue** before allocating an instance, and `stream-test` timed out at 240 s of `WaitingForResources`. This is not a bug in Green-NX or in this tool — it is the ad-supported tier's real queue depth for the region+time.

Workarounds, in ascending cost/reliability:

1. Retry off-peak (Brazil overnight / weekday early morning) — queue depths can drop to minutes.
2. Activate an Xbox Game Pass trial (often 14-day free) to switch the offering to `xgpuweb`, which allocates near-instantly.
3. Change region via VPN (not recommended — violates ToS + confounds bitstream analysis).

P5 stays blocked on this dimension until one of the above is satisfied. Everything else in `xcloud-capture` (setup, login, games, ids) works independently.

## Security notes

- Ad-supported title testing (`--ads`) is preferred over Game Pass titles for early captures — a shorter, capped session works fine for bitstream characterization and does not consume a paid subscription slot.
- Sessions self-cleanup on tool exit (`GssvSession::cleanup_stale_sessions`).
- The captured `.h264` is a raw bitstream of the ~30 seconds of streaming needed to characterize the codec — it is not intended for playback or redistribution.

## P5.4 status (2026-09-07)

libpeer PC-side builded — same pin as Green-NX (`9319aa434c...`) with mbedTLS 3.4.0 + libsrtp2 + usrsctp + cJSON as static libs.

- Path: `~/libpeer/build-pc/dist/lib/{libpeer.a, libmbedcrypto.a, libmbedtls.a, libmbedx509.a, libsrtp2.a, libusrsctp.a, libcjson.a}`
- Headers: `~/libpeer/include/peer.h`, `~/libpeer/include/peer_connection.h`
- Example binary `~/libpeer/build-pc/examples/generic/sample` built (3.1 MiB) — proves link chain works.

Key API for GOLDEN STREAM capture:

```c
PeerConfiguration config = {
    .video_codec = CODEC_H264,
    .datachannel = DATA_CHANNEL_BINARY,
    .onvideotrack = video_nal_callback,   // <— fires per received NAL
    .onaudiotrack = audio_frame_callback,
    .on_request_keyframe = keyframe_callback,
    .ice_servers = {{.urls = "stun:stun.l.google.com:19302"}, ...},
};
pc = peer_connection_create(&config);
peer_connection_set_local_description(pc, our_offer_sdp, SDP_TYPE_OFFER);
answer = xcloud_exchange_sdp(session, our_offer_sdp);   // HTTP POST /sdp
peer_connection_set_remote_description(pc, answer, SDP_TYPE_ANSWER);
// ICE plumbing via peer_connection_onicecandidate + xcloud_send_ice
```

The `video_nal_callback` writes each NAL to `stream.h264` prefixed with Annex-B start code (`00 00 00 01`). That file IS the GOLDEN STREAM.

Signaling reached Provisioned successfully (log at `output/FORTNITE-20260907-164320/greennx-stream-test.log`) — Green-NX PC drove state machine to Provisioned in ~3:36 (216 iter × 1s = 216s queue, then Provisioning 7s, then Provisioned).
