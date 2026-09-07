# ADR-001 — Cross-toolchain choice

**Status:** accepted (2026-09-07)
**Owner:** Sergio Lima

## Context

Xbox 360 = PowerPC 64-bit BE (Xenon) with VMX128. No off-the-shelf Linux distro cross-compiler produces XEX binaries. Two mature paths exist:

1. **libxenon / Free60** — legacy toolchain (`xenon-gcc` = GCC 4.6.4, binutils 2.22, newlib 1.20). Bootstrap script `build_tool.sh` provided. Battle-tested — XBMC-360, XeUI, Iron Wolf, dozens of homebrew XEXs. Abandoned upstream but stable.
2. **OpenXeChain** — modern LLVM + newlib + xecorelib + SynthXEX. Under active development, direction of the future. Heavy bootstrap (LLVM source ~3GB, ~6-8h compile on 4 cores).

## Decision

**Hybrid.** Start with libxenon for P1 (minimal XEX in hours). Migrate to OpenXeChain later — timing driven by whether VMX128 codegen from GCC 4.6 is acceptable for the H.264 hot path. Decision revisited at P8 (VMX completion) based on profiler data.

## Consequences

- P1 delivered in days, not weeks.
- Any GPL-2 code linked against libxenon runtime propagates GPL to derived work — call sites must be tracked in `THIRD_PARTY.md`.
- VMX128 intrinsics support in GCC 4.6 is thin — may need inline asm or migration when P8 hotspots demand it.
- OpenXeChain build effort deferred but not skipped. Docker image for OpenXeChain will be provisioned in parallel once libxenon toolchain is stable.

## Related

- Spec §29 P0, §34, §35 (libxenon reference)
- Future: ADR-00X on OpenXeChain migration trigger
