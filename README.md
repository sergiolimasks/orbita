# orbita

Native Xbox Cloud Gaming (xCloud) client for the **Xbox 360 (RGH/JTAG)**.
Runs 100% on the console — login, catalog, session, WebRTC, H.264 + Opus
decode and controller input — with no PC in the middle.

> orbita is an independent, unofficial project. It is not affiliated with,
> endorsed by or sponsored by Microsoft. Xbox, Xbox 360, Xbox Cloud Gaming and
> Game Pass are trademarks of Microsoft. You need your own account and an
> active subscription that includes cloud gaming.

**Status:** closed beta (v0.20.x). Public beta starts at 0.22.0.
Site, downloads and feature status: <https://x360.pistatrack.com>

## Requirements

- Xbox 360 with RGH/JTAG
- XeLL Reloaded v0.993 or newer, launched from Aurora via the *XeLL Launcher* app
- Wired network (Wi-Fi is not supported)
- Up to 4 Xbox 360 controllers (Xbox Series controllers are in testing)

## What works

- Microsoft account login on the console (code on screen, sign in from a phone)
- xCloud-style menu with covers, recent games, popular and new titles
- 720p60 H.264 streaming, Opus audio, up to 4 controllers
- Keep the session alive (Back+Start), restart/shutdown from the menu
- In-app updates (signed manifest, beta/stable channels)

See the live [status page](https://x360.pistatrack.com/status) for details.

## Repository layout

| Path | What |
|---|---|
| `app/h264play/` | The console app (libxenon ELF). Module docs: `MENU-UI.md`, `CATALOG-COVERS.md`, `SESSION-STAGE3.md`, `WEBRTC-STAGE4.md`, `STANDALONE-STAGE5.md`, `UPDATE.md`, `BETA-GATE.md`, `LOGS-UPLOAD.md`, `GIP-CONTROLLER.md` |
| `app/h264play/hosttest/` | Host-side unit/E2E tests (run on Linux) |
| `deps/` | Build scripts for Xenon ports of mbedTLS, libopus, libpeer/usrsctp/libsrtp |
| `web/` | Beta site and admin panel (Flask + Postgres) |
| `tools/` | Release signing/publishing, capture and analysis tools |
| `brand/` | orbita logo and palette |
| `docs/` | ADRs, protocols, test results |

## Building

Linux host, libxenon toolchain (`xenon-gcc` 9.2) — see `docs/adr/ADR-001-toolchain.md`.

```sh
source scripts/env.sh                 # puts devkitxenon on PATH
deps/build-mbedtls-xenon.sh
deps/build-opus-xenon.sh
deps/webrtc-xenon/build.sh xenon
# FFmpeg (libavcodec H.264) for Xenon: see docs/adr/ADR-007-h264-decoder-base.md
cd app/h264play && make               # -> h264play.elf32 (boot from XeLL as xenon.elf)
```

Build options (`BETA`, `GPU`, `AUDIO`, `DECODE_THREADS`, …) are documented in
`app/h264play/Makefile`. No Microsoft SDK (XDK) is used or required.

## License

GPL-3.0 — see [LICENSE](LICENSE). Third-party code and its licenses are listed
in [THIRD_PARTY.md](THIRD_PARTY.md).
