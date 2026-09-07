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
