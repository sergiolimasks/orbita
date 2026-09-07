# Changelog

## Unreleased

### 2026-09-07 — P0 bootstrap

- Renamed workspace `#X360` → `XCloud360` (ADR-005).
- Init isolated git repo.
- Seeded directory layout per spec §38.
- ADRs 001–005 recorded (toolchain, 720p60 target, WebRTC stack, VMX128 plan, workspace).
- README, THIRD_PARTY, .gitignore, dev.local.example.json seeded.
- Environment audit complete. Host toolchain OK (gcc 13.3, git, python, node, bun, lftp). Missing on host: clang, cmake, ninja, autoconf/automake/libtool, docker, qemu-ppc. No cross-toolchain PPC/Xenon installed.
- Xbox 360 RGH at 192.168.15.47 confirmed: TCP:21 open (ICMP filtered — expected).

### 2026-09-07 15:20 — P1 hwtest first build

- Toolchain bootstrap complete: xenon-gcc 9.2.0, binutils 2.32, newlib 3.1.0,
  libxenon.a (2.9 MB) installed at ~/devkitxenon (per ADR-001).
- `app/hwtest/hwtest.elf32`: **198,748 bytes**, sha256
  `2d662df1b21071e5cc6849cfe35fba4dfdc888c456e34a6886275554de2d7d2e`.
- ELF32 PPC BE EXEC, entry `0x80002500`, LOAD base `0x80000000` (libxenon convention).
- Sections: .text 128 KiB, .data 30 KiB, .rodata 12 KiB, .bss 5 MiB (framebuffer + lwIP).
- 662 defined symbols, 0 unresolved externals — clean link.
- Deployed to Xbox FTP: `Hdd1/Apps/XCloud360/hwtest.elf32` (198,748 bytes on-console).
- HTTP netboot server up at `http://192.168.15.64:8032/build/xenon.elf` for XeLL Reloaded.

### 2026-09-07 15:49 — P1.5 inputdiag + ADR-006 (wireless RF gap)

- **Discovery**: internal RF module IS USB-connected via southbridge (per Free60
  wiki), but libxenon usbdevs.c only lists 6 controller VID/PIDs — missing
  `0x045e:0x0719` (Wireless Gaming Receiver) among others. **Bare-metal
  input via internal RF is unverified until inputdiag runs on hardware.**
- `app/inputdiag/inputdiag.elf32` — 187,444 bytes, sha256
  `921d800c28b7d758efd26c137b78ca736ebc8ba8dddf96052b4ba764b8d61a3f`.
  Enumerates every USB device on every OHCI bus (VID/PID/class/driver-bound),
  cross-refs known IDs, polls get_controller_data + SMC IR for 20s, reports.
- hwtest.elf32 rebuilt with input warning + reference to ADR-006. New sha256
  `f0049b155ca265aa5bb21e572b9f57cd1426ee955e5eaa48886e850a75f555e7`.
- Both artifacts deployed via FTP to `Hdd1/Apps/XCloud360/`. Available via
  HTTP netboot at `http://192.168.15.64:8032/build/{xenon,inputdiag}.elf`.
- ADR-006 records the code evidence, the hardware evidence (Free60 wiki),
  what we still don't know, and the branch table on next steps based on
  inputdiag outcome.

### 2026-09-07 15:56 — P1.5-INPUT-001 (Machine Check) forensics + inputdiag-safe

- **Crash on hardware**: original inputdiag.elf32 hit exception vector 0x200
  (Machine Check) with DAR=0xC8000000 (MMIO/NAND region) before RF module
  enumeration data was captured. Symbolized via xenon-addr2line:
  fault inside `_vfprintf_r` at `vfprintf.c:1528`, one instruction before
  a `bl strlen` — classic case of `printf("%s", ...)` with a dangling
  pointer whose strlen scan walks into MMIO.
- **Root cause**: original inputdiag included `<usb/usbd.h>` and walked
  `usb_buses[b]->ub_devices[addr]` from user code, dereferencing internal
  fields at unsafe times. The libxenon `browser` example uses ONLY the
  public API (`usb_init` / `usb_do_poll` / `get_controller_data`) and
  works — so `inputdiag-safe` mirrors that exactly.
- **Preserved for post-mortem**:
  `docs/test-results/crash-P1.5-INPUT-001/` contains unstripped ELF,
  stripped ELF, map file, metadata, POSTMORTEM.md with full symbolization.
- **`app/inputdiag-safe/inputdiag-safe.elf32`**: 185,564 bytes, sha256
  `5ed5a246516fa0aefd7b7aa0e465b6552072ed1cf1ad7ea68b90d821888bb959`.
  Public API only. Every printf < 4 args. No manual USB internals iter.
  Same 20s pad-press + SMC IR window + in-band diagnosis.
- Deployed via FTP to `Hdd1/Apps/XCloud360/inputdiag-safe.elf32` +
  HTTP netboot at `http://192.168.15.64:8032/build/inputdiag-safe.elf`.

### 2026-09-07 16:07 — P1.5 RESOLVED (usbctrl binds internal-RF pad OK)

- Hardware test with `inputdiag-safe.elf32` **PASSED**. 4 photos captured
  by Sergio archived at `docs/test-results/P1.5-inputdiag-safe/photos/`.
- **Key line from console**:
  `USB bus 1 device 2: vendor 045E product 02AA class FF: Xbox 360 Controller`
- **pad0 events = 35** in the 20 s window; A/B/X/Y/Back/Start all captured.
- SUMMARY block on-screen: `DIAGNOSIS: usbctrl / SMC PATH WORKING.`
- **Finding**: `045E:02AA` is the *unified* PID a wireless pad exposes
  via ANY receiver — external USB or internal RF module. libxenon
  already lists this PID in `usbdevs.c`; no patch required. bare-metal
  input path fully functional. ADR-006 closed as RESOLVED-POSITIVE.
- hwtest rebuilt with softened input note (points at ADR-006 resolution).

### 2026-09-07 16:32 — P4 rendertest (framebuffer @720p, no shader)

- `app/rendertest/rendertest.elf32`: 185,596 bytes, sha256
  `38c64643c998fa8ef9c42ade9d8446af8cc99db2d921d4a0e1b90a0a3ebdc5c2`.
- Forces `VIDEO_MODE_HDMI_720P` (not AUTO), paints SMPTE 7 color bars
  via CPU-side `console_pset()`, measures full-frame repaint time
  vs 60Hz budget (16667 us). Reports FITS / OVER verdict on screen.
- P5 blocker (Microsoft ad-supported queue) means shader path (needed
  for YUV→RGB from a real bitstream) is deferred to P4.5. Rationale:
  Xbox 360 shader bytecode requires proprietary shadercompiler.exe
  (Windows + XNA 3.0 + VS 2008). No open-source compiler exists.
  Better to defer shader work until real YUV samples exist to test
  against, rather than write a shader against imagined input.
- Deployed via FTP + HTTP netboot.
- docs/test-protocols/P4-rendertest.md added with expected output +
  PASS/FAIL matrix + what to photograph.
