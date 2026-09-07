# XCloud360

Native Xbox Cloud Gaming client for Xbox 360 (RGH).

Target: 1280x720 @ 60 FPS, H.264 + Opus over WebRTC, low-latency input, no PC intermediary.

## Status

P0 — bootstrap. See `docs/adr/` for architectural decisions.

## Layout

See section 38 of the project spec. Top-level modules:

- `platform/xbox360/` — hardware abstraction (net, threads, timer, controller, fs, audio, Xenos GPU)
- `xcloud/` — auth, catalog, session, signaling, protocol
- `webrtc/` — ICE, DTLS, SRTP, SCTP, RTCP
- `video/` — RTP depacketizer, H.264 decoder, VMX128 kernels, Xenos renderer
- `audio/` — Opus decode, Xbox audio out
- `input/` — controller polling → xCloud input serialization
- `diagnostics/` — profiler overlay, logging
- `app/` — UI shell
- `tools/xcloud-capture/` — PC-side reference stream capture (Golden Stream)
- `tools/stream-analyzer/` — bitstream inspection

## Build

Not yet — cross-toolchain provisioning in progress (see `docs/adr/ADR-001-toolchain.md`).

## Deploy

Copy `dev.local.example.json` → `dev.local.json`, fill Xbox FTP creds.

`scripts/deploy-xbox` (TBD) will compile + upload via lftp to `Hdd1:/Apps/XCloud360/`.

## References

See `THIRD_PARTY.md`.
