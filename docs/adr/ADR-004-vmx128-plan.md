# ADR-004 — VMX128 plan for H.264 hot path

**Status:** planned (execute at P7/P8)

## Context

Xenon has VMX128 — a superset of AltiVec with more registers and Xbox-specific instructions. GCC 4.6 (libxenon default) support for VMX128 intrinsics is incomplete. XBMC-360 has partial implementations in `libraries/ffmpeg/libavcodec/xbox360/` — quality unknown.

## Decision

1. **P7 audit** — before writing any new VMX code, instrument existing paths with call counters (`vmx_idct_calls++`, etc.) per spec §15. Determine which kernels actually execute at runtime.
2. **Profile before optimize** — hotspots dictate priority (§32). Do not optimize kernels that do not appear in the profile.
3. **Sources of algorithm** — FFmpeg upstream `libavcodec/ppc/` AltiVec kernels (h264chroma, h264dsp, h264qpel). NOT direct copy — port intrinsic-by-intrinsic to VMX128 because instruction sets differ.
4. **Fallback plans** in order — if VMX128 codegen insufficient: (a) inline asm; (b) hand-written .S files; (c) Xenos GPU assist (§43) for parallelizable stages (inverse transforms, MC, deblock, color conversion).

## H.264 kernel matrix (populate as work progresses)

| Kernel | C ref | PPC AltiVec ref | VMX128 impl | Called at runtime | Perf (ms) | Correct |
|---|---|---|---|---|---|---|
| IDCT 4x4 | ffmpeg | ffmpeg/ppc | — | — | — | — |
| IDCT 8x8 | ffmpeg | ffmpeg/ppc | — | — | — | — |
| luma MC | ffmpeg | ffmpeg/ppc | — | — | — | — |
| chroma MC | ffmpeg | ffmpeg/ppc | XBMC-360 partial | — | — | — |
| QPEL | ffmpeg | ffmpeg/ppc | XBMC-360 partial | — | — | — |
| deblock V | ffmpeg | ffmpeg/ppc | — | — | — | — |
| deblock H | ffmpeg | ffmpeg/ppc | — | — | — | — |
| weighted pred | ffmpeg | ffmpeg/ppc | — | — | — | — |
| CAVLC | ffmpeg | ffmpeg | — | — | — | — |
| CABAC | ffmpeg | ffmpeg | — | — | — | — |

## Related

- Spec §12, §13, §14, §15, §32, §42, §43
