# h264play: Xenos GPU display backend (v0.15.0-gpu)

YUV420 -> RGB now runs in a Xenos pixel shader instead of on two CPU hardware threads.

**Status: compiled and linked, and the host-side scheduling test passes. It has never run on an Xbox.** Read "Verification" below before trusting any of it.

## Build knobs

| knob | default | effect |
|---|---|---|
| `GPU` | `1` | `1` = GPU backend (`-DXCLOUD360_GPU=1`, links with `-Wl,--wrap=Xe_Fatal`, VERSION `0.15.0-gpu`). `0` = the v0.14.4 CPU path, unchanged. |
| `VSYNC` | `0` | GPU only. `1` = the scanout flip spins until `Xe_IsVBlank()` (at most 20 ms on the main thread). |
| `DECODE_THREADS` | `4` with GPU=1, `3` with GPU=0 | frame threads (one hardware thread each) |
| `DISPLAY_DIVISOR` | `2` | unchanged. With the GPU path, `1` is now worth trying. |

`build/` is shared, so run `make clean` when you switch `GPU`.

`GPU=0` produces a byte-identical `h264play.elf32` to the unmodified tree (sha256 `81dd6bf9…f26a`, checked).

## Thread map

| hw thread | GPU=1 | GPU=0 (v0.14.4) |
|---|---|---|
| 0 | main: decode dispatch, texture upload, all `Xe_*` calls | main: decode dispatch, memcpy into ring |
| 1 | network (lwIP), no hook | network + bottom-half convert (idle hook) |
| 2 | decode | convert worker (top half) |
| 3, 4, 5 | decode | decode |

`deps/xenon-pthread/xenon_pthread.c` `k_slots`:
- With `XCLOUD360_GPU`: `{4, 5, 2, 3, 1}`. Slot 1 is normally taken by the network thread and is free in embedded mode.
- Otherwise: still `{4, 1, 5, 3}`.

`main.c` clamps `thread_count` to the free hardware threads: 6 − main − network − (worker, if the CPU path is used). Without the clamp, `avcodec_open2` would fail when `pthread_create` runs out of slots.

## Per-frame flow (main thread, `source/gpu_present.c`)

Frame N runs these steps:

1. **`gpu_present_poll()`**
   - Reads SCRATCH0 from the writeback word (`xe->rb + 0x10100`, after a dcbf).
   - Flips the scanout to the newest frame that has finished.
   - Calls `Xe_VBReclaim()` once all submitted work is done.
   - It never blocks. It is also called at the top of every main-loop iteration, which means once per AU and every 1 ms on the TCP idle path.
2. **Wait, normally 0 µs.**
   - Texture set `N%2` must no longer be sampled by frame N−2.
   - Scanout buffer `N%3` must not be on screen or still pending.
3. **Upload.**
   - `Xe_Surface_LockRect` → `memcpy` each row (linear `XE_FMT_8`, row stride `wpitch`; Y is 1280×736 allocated, U/V 640×384) → `Xe_Surface_Unlock`.
   - The unlock does a dcbst of the range and emits a GPU cache-invalidate packet.
   - Texture memory is cached (0x8… segment), so the flush is required. Never write it through the uncached alias.
4. **Draw.**
   - `Xe_InvalidateState` (opaque ONE/ZERO blend, no alpha test), Z off, cull none.
   - xmplayer's VS/PS. Samplers: s0 = Y, s1 = U, s2 = V, with CLAMP addressing and bilinear filtering.
   - One fullscreen 3-vertex RECTLIST.
   - `Xe_ResolveInto(scan[N%3], COLOR, CLEAR_COLOR|CLEAR_DS)`.
5. **Fence and kick.**
   - `WAIT_UNTIL(idle)` (0x1720 = 0x20000), then SCRATCH0 (0x15e0) = `xe->frameidx++`, then `Xe_Execute`.
   - The CPU does not wait. The fence uses the same counter as `Xe_Sync()`, so an occasional `Xe_Sync` stays consistent.

Why 2 texture sets and 3 scanout buffers:
- The CPU fills set N+1 while the GPU samples set N.
- Once N−2 has finished, the screen shows N−2 or N−1. The resolve of N therefore never lands in the displayed buffer, even when the flip of N−1 hasn't happened yet. This is a triple-buffered scanout, not xmplayer's double buffer. xmplayer flips before kicking and so shows every frame one interval late.

Memory:
- About 14.2 MB of heap via `memalign`: 3 × 3.77 MB scanout + 2 × 1.43 MB YUV.
- Plus libxenon's 4 MB ring.
- bss grows by only about 10 KB.

**Latency.** The GPU work (draw plus resolve, estimated around 1 ms, unmeasured) runs in the background. The flip happens at the first poll after it finishes:
- At most about 1 ms later on the TCP idle path.
- Longer if main is blocked inside `avcodec_decode_video2`.
- The flip is a raw register write with no vblank wait (`VSYNC=0`). Whether the display controller latches it at vblank is **unknown**. If tearing shows, try `VSYNC=1`.

## Console / init / fallback

- **Init timing.** `gpu_present_init()` runs right after `fb_direct_init()`, before any hardware thread exists.
- **Init steps.** `Xe_Init`, then the surfaces, then the render target (our own scanout surface, not `&xe->tex_fb`), then the shaders and VFetch patch, then `edram_init`, then the VB, then a **test fence** that must complete within 500 ms.
- **Fallback to the CPU path.** Each of these triggers `printf("[gpu] init FAILED: …")` and the normal worker/hook path:
  - the scanout is larger than 1280×720 (EDRAM budget);
  - `edram_init`'s `Xe_Fatal` fires (caught through `--wrap=Xe_Fatal` plus `longjmp`, only during init);
  - the test fence never completes.
- **Fatal errors inside `xe.o` are not caught.** For example, out of memory in `Xe_pAlloc` still aborts, as before. Those calls are internal to the object, and `--wrap` cannot intercept them. Checked with objdump: `edram_init` calls `__wrap_Xe_Fatal`, while `Xe_ResolveInto` calls the real `Xe_Fatal`.
- **Console visibility.** Boot messages stay visible until the first frame. On the first flip, `console_close()` runs, so printf stops drawing into an fb that is no longer scanned out. That also removes the cost of `console_putch` doing a dcbst over the whole fb for every character. When the stream ends, `gpu_present_finish()` waits for the GPU, restores `default_fb` and calls `console_open()`, so `==== RESULT ====` is visible.
- **No worker wait in GPU mode.** The end-of-stream code no longer waits for hardware thread 2 when the GPU is used. Thread 2 is a decoder there and never exits, so the old loop would hang.

## Telemetry

The STAT keys and their order are unchanged. Their meaning with GPU=1:

| key | meaning with GPU=1 |
|---|---|
| `prod_ms` | whole `gpu_present_frame()` call (poll + wait + upload + submit) |
| `cvt_ms` | upload only: plane memcpy + Unlock (dcbst) |
| `bot_ms` | always 0 |

Extra keys are appended **at the end**, only with GPU=1:
- `gpu=1`
- `gwait_ms` (CPU blocked on the GPU per frame; should be about 0)
- `flip_ms` (submit → flip)
- `flips`
- `sup` (frames that finished but were superseded before being shown)
- `gto` (fence timeouts; non-zero means the GPU hung)

capture-rtp prints the line verbatim. The buffer was raised from 200 to 240 bytes, which matches `STAT_MAX`. The console `wF=` field counts flips. TRACE_LATENCY now stamps `t_flipped` at the scanout flip.

## Verification

**Verified on the devbox:**
- `make GPU=1` and `make GPU=0` build with no warnings. So do the variants `TRACE_LATENCY=1`, `VSYNC=1`, `DECODE_THREADS=1` and `DISPLAY_DIVISOR=1 DECODE_THREADS=3`.
- `gpu_present.c` is also clean with `-Wextra`.
- The `GPU=0` `.elf32` is byte-identical to a build of the pristine tree.
- The embedded `xmp_ps`/`xmp_vs` in the GPU=1 ELF are word-for-word identical to `vo_xenon.c`. The PS literals decode to the BT.601 coefficients: 1.164, 1.596, −0.391, −0.813, 2.018, offsets −0.8707/−1.0817.
- `hosttest/run.sh` compiles `gpu_present.c` on x86 against mocks (in-order fake GPU, simulated clock) and checks the following across 6 load scenarios (33 000 frames, random GPU latency 0.05–40 ms, random frame intervals, resolution changes):
  - the CPU never writes a texture set the GPU is sampling;
  - a resolve never targets the displayed or in-flight buffer;
  - flips only show finished frames, in order;
  - VB reclaim only happens when the GPU is idle.
  - Also covered: GPU hang (waits time out, no infinite spin), the `edram_init` `Xe_Fatal` fallback via `--wrap`, a dead CP at init, and 1080p refusal.

**Only hardware can verify:**
- whether the GPU path works at all: colors, orientation, sharpness;
- the shader's half-texel behaviour (if it looks soft, try Y `use_filtering = 0`);
- real GPU and upload timings;
- whether the scanout register write tears;
- that `edram_init` and `Xe_Init` coexist with the earlier `xenos_init`/`console_init` the way they do in xmplayer;
- the heap headroom for about 18 MB of GPU buffers plus a fourth decode thread;
- the BT.601-versus-BT.709 question, which affects both paths equally.

**Unchanged:** the decoder. The known frame-threading error in the embedded stream near frame 117 is untouched. DECODE_THREADS=4 changes frame-thread timing, so watch for it.

## Hardware test plan

1. Build with `make embedded` and `make GPU=1`, boot, and compare colors and geometry with a `GPU=0` build.
2. Check STAT `cvt_ms` (upload), `gwait_ms` ≈ 0, `flip_ms`, `gto=0`.
3. Try `DISPLAY_DIVISOR=1`, and `VSYNC=1` if it tears.
4. Later: zero-copy decode straight into texture memory (custom `get_buffer`) to drop the upload.
