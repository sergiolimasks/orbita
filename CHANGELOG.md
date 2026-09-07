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
