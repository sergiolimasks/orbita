# TEST P4 v2 — rendertest2 (corrected)

## Why v2

`rendertest.elf32` (v1) had a methodological bug: it used `console_get_dimensions()` as the pixel size of the framebuffer, but that call returns **text cells** (148×41 at 720p with libxenon's 8×16 font). The v1 loop drew a 148×9 strip in the top-left corner and reported `full 1280x720 CPU repaint = 36 us` — that number matches ~1332 `console_pset()` calls at ~27 ns each, not a real 921,600-pixel repaint.

The v1 `FITS -> CPU upload viable` verdict is invalid — **discard it**.

## v2 fixes

- Hardcodes pixel bounds to `1280×720` (the resolution `VIDEO_MODE_HDMI_720P` forces).
- Reports both **text cells** (as returned by the API, for transparency) and the **pixel bounds** it actually paints, so the reader can see they differ.
- Reserves 32 px (= 2 text rows) at the top for the on-screen overlay.
- Paints a genuine 1280×(720−32) = **880,640 pixels** of SMPTE bars.
- Bench per-`pset` overhead separately (10k samples on the text strip we're overwriting anyway).
- Reports full-frame paint time + throughput in MB/s + verdict.
- Paints 4 sentinel corner pixels in unique colors (red TL / green TR / blue BL / yellow BR) for visual verification of pixel bounds.

## Artifact

- Local: `app/rendertest2/rendertest2.elf32` — 185,612 bytes
- SHA-256: `d032ccc78b0b2512574bfcb06f9b768c9a41092156d939764749ceccd916d9b3`
- Remote (FTP): `Hdd1/Apps/XCloud360/rendertest2.elf32`
- HTTP netboot: `http://192.168.15.64:8032/build/rendertest2.elf`

## Expected on-screen output

Top overlay (rows 0..31):

```
==========================================
 XCloud360 rendertest v2 (P4 corrected)
 build: 0.0.2-rendertest
 built: Sep  7 2026
==========================================
[XENOS] init returned; is_initialized=1 overscan=<0|1>
[FB] console text cells: 148x41  (v1 bug thought these were px)
[FB] pixel bounds enforced by rendertest v2: 1280x720
[FB] top strip reserved for overlay: 32 px
[FB] paint region: 688 rows x 1280 cols = 880640 pixels
[BENCH] measuring pset() overhead (10000 samples, top strip)...
[BENCH] 10k pset calls = <N> us (~<N> ns/call)
[PAINT] painting SMPTE bars full 1280x688 (880640 pixels, 3522560 bytes)...
[PAINT] done in <N> us (~<N> ms) -> <N> MB/s effective
[PAINT] 60Hz budget = 16667 us. verdict: FITS -> CPU upload viable
                                       OR OVER -> shader path required
[SENTINEL] 4 corner pixels painted (visual inspection only)
[USB] init...
[USB] settle done.
```

Rest of screen (rows 32..719, all 1280 columns):

**7 vertical SMPTE bars** — **WHITE | YELLOW | CYAN | GREEN | MAGENTA | RED | BLUE**, each ~183 px wide, covering essentially the full screen.

**Sentinel corners** (visually):
- top-left of bars region (0, 32): red pixel
- top-right (1279, 32): green pixel
- bottom-left (0, 719): blue pixel
- bottom-right (1279, 719): yellow pixel

## What to photograph

- One shot showing text overlay + full-screen bars (most of the screen should be color, not black).
- Note the exact numbers on these two lines:
  - `[PAINT] done in <N> us (~<N> ms) -> <N> MB/s effective`
  - `[PAINT] 60Hz budget = 16667 us. verdict: <FITS|OVER>`

## Pass / fail matrix

| Line | PASS if | FAIL means |
|---|---|---|
| `is_initialized=1` | 1 | Xenos init failed |
| bars fill nearly full screen | 7 bars cover rows 32..719, all 1280 cols | pset math or bounds wrong |
| bar order left→right W Y C G M R B | correct | color-byte order or band math wrong |
| all 4 sentinel corners visible | red/green/blue/yellow single pixels visible at the corners | bounds off by pixel(s) |
| paint time reported as ms not us | reasonable — 800k pset calls at ~30 ns each ≈ 24 ms | timing bug still present |

## What this measurement is worth

The v2 number is a **worst-case upper bound** on CPU-only framebuffer paint. If it comes back at ~20-30 ms per full frame (~28-42 fps), that's usable as a fallback baseline. If <16.67 ms it CAN sustain 60 Hz on CPU — but this does not endorse CPU YUV→RGB as production architecture (Sergio's rule: production must use Xenos shader path so the CPU is preserved for H.264/WebRTC/Opus/input).

If it comes back >>16.67 ms, that's harder evidence that the shader path is mandatory.
