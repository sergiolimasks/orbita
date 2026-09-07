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
