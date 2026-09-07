# TEST P4 — rendertest (framebuffer @720p, no shader)

## Objective

Prove that:
1. Xenos can be forced to **VIDEO_MODE_HDMI_720P** and initializes (`xenos_is_initialized() == 1`).
2. Console framebuffer paints at **1280x720** at the pixel level.
3. CPU-side full-frame pixel-writes are timed so we know if CPU upload is feasible at 60Hz or if the shader path is mandatory (per spec §21).
4. HDMI negotiates 720p and the console outputs a coherent SMPTE color-bar pattern.

**Not tested here (deferred):** vsync-locked 60Hz present, YUV texture upload, YUV→RGB shader. Those need `Xe_*` API + shader bytecode (Xbox 360 SDK's `shadercompiler.exe`) — no open-source path yet. Will revisit at P4.5 once GOLDEN STREAM (P5) hands us real YUV.

## Artifact

- Local: `app/rendertest/rendertest.elf32` — 185,596 bytes
- SHA-256: `38c64643c998fa8ef9c42ade9d8446af8cc99db2d921d4a0e1b90a0a3ebdc5c2`
- Remote (FTP): `Hdd1/Apps/XCloud360/rendertest.elf32`
- HTTP netboot: `http://192.168.15.64:8032/build/rendertest.elf`

## Deploy

Same 3 paths as P1/P1.5. Recommended: HTTP netboot via XeLL — swap URL to `rendertest.elf`.

## Expected screen output

**Top ~32 lines** = text overlay:

```
========================================
 XCloud360 rendertest (P4)
 build: 0.0.1-rendertest
 built: Sep  7 2026
========================================
[XENOS] init returned; is_initialized=1
[XENOS] overscan flag: <0 or 1>
[FB] console dimensions: 1280 x 720
[PATTERN] painting SMPTE color bars (this takes a moment)...
[PATTERN] paint done in <N> us (~<N> ms)
[LOOP] measuring 300-iteration spin rate...
[LOOP] 300 empty loops = <N> us (~<N> ns/loop)
[BUDGET] timing a full-frame CPU repaint (worst case)...
[BUDGET] full 1280x720 CPU repaint = <N> us (<N> ms)
[BUDGET] 60Hz budget = 16667 us. FITS -> CPU upload viable
                                 OR OVER -> shader path required
[USB] init...
[USB] settle done.
========================================
 PATTERN VISIBLE ON TV = renderer PASS
 Press BACK to exit.
========================================
```

**Rest of the screen (rows 32..719)** = **7 vertical color bars**, left to right:

**WHITE | YELLOW | CYAN | GREEN | MAGENTA | RED | BLUE**

Each bar ~183 pixels wide (1280 / 7 ≈ 182.86).

## User actions

1. Watch the TV.
2. Confirm you see the 7 color bars in the correct order (white on the LEFT, blue on the RIGHT).
3. **Photograph the screen** — one shot showing text overlay + bars.
4. Note the `[BUDGET] full 1280x720 CPU repaint` value and whether it says `FITS` or `OVER`.
5. Press **BACK** to exit.

## Duration

~2–5 s (paint operation dominates; text overlay dominates the read time).

## Pass / fail matrix

| Line | PASS if | FAIL means |
|---|---|---|
| `is_initialized=1` | 1 | Xenos init failed |
| `console dimensions: 1280 x 720` | exactly 1280x720 | HDMI negotiated a different mode; force with `VIDEO_MODE_HDMI_720P` may have failed |
| Bars visible in correct order | 7 bands left→right WYCGMR B | framebuffer stride or tiled-layout math is off |
| No visible tearing / corruption | clean image | cache flush (memdcbst) may be missing on this build |
| BUDGET says `FITS` | <16667 us | CPU-only upload works; if `OVER`, shader path required for real streaming |

## What to send back

- **1 photo** of the TV (bars + text).
- Text line `[BUDGET] full 1280x720 CPU repaint = <N> us (<N> ms)`.
- Whether the last verdict was `FITS` or `OVER`.

## After

- If PASS across the board → advance to P4.5 (Xe shader path + YUV textures) once GOLDEN STREAM lands.
- If bar order is scrambled → fix `console_pset` tiled-layout math or byte order.
- If dimensions are wrong → investigate HDMI negotiation / `xenos_init` param.
