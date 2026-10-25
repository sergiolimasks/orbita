/*
 * XCloud360 — h264play GPU display backend (Track B).
 *
 * YUV420 -> RGB on the Xenos GPU with xmplayer's precompiled BT.601 pixel
 * shader. Only built with `make GPU=1` (defines XCLOUD360_GPU).
 *
 * THREADING: every function here issues Xe_* calls or touches the
 * XenosDevice, which has no locking. Call them from the MAIN thread only.
 *
 * See app/h264play/GPU-BACKEND.md for the design.
 */
#ifndef XCLOUD360_GPU_PRESENT_H
#define XCLOUD360_GPU_PRESENT_H

#include <stdint.h>

/* Cumulative counters (since init). The caller diffs them per report. */
typedef struct {
    uint64_t frames;        /* frames submitted to the GPU */
    uint64_t upload_us;     /* CPU time copying planes into textures + Unlock (dcbst) */
    uint64_t wait_us;       /* CPU time blocked waiting for the GPU (set/scanout reuse) */
    uint64_t flips;         /* scanout flips done */
    uint64_t flip_lag_us;   /* sum of (flip time - submit time) over flips */
    uint32_t superseded;    /* finished frames never shown (a newer one finished first) */
    uint32_t timeouts;      /* GPU fence waits that timed out (GPU hang?) */
} gpu_present_stats_t;

/* Init Xenos device, EDRAM, shaders, 2 YUV texture sets (tex_w x tex_h luma)
 * and 3 tiled scanout buffers. Must run before any other hardware thread
 * is started. The console framebuffer stays on screen until the first
 * flip. Returns 0 on success; <0 on failure (the caller must then use the
 * CPU path; a reason has already been printf'd).
 * v0.16: re-entrant. Calling it again after gpu_present_finish() does NOT
 * redo Xe_Init or allocate anything: it only re-arms the scheduling state
 * (texture sets, scanout buffers, fences) so the next frame starts clean.
 * Same tex_w/tex_h required. A call while already active is a no-op. */
int  gpu_present_init(int tex_w, int tex_h);

/* Upload one decoded frame (planes as given by libavcodec) into the next
 * free texture set, draw, resolve into a free scanout buffer and kick the
 * GPU. Does NOT wait for the GPU, except when the texture set it needs is
 * still being sampled by frame N-2 (or a scanout buffer is still on screen),
 * which should not happen at <= 60 fps. `tag` is handed back through the
 * flip callback (latency tracing). */
void gpu_present_frame(uint8_t *const data[3], const int linesize[3],
                       int w, int h, uint64_t tag);

/* Cheap, non-blocking: if submitted frames have finished on the GPU, flip
 * the scanout to the newest one and recycle libxenon's per-frame vertex
 * buffers. Call it often from the main loop. */
void gpu_present_poll(void);

/* Optional (TRACE_LATENCY): called from gpu_present_poll() on each flip. */
void gpu_present_set_flip_cb(void (*cb)(uint64_t tag, uint64_t t_flip_us));

/* End of stream: wait for the GPU, give the scanout back to the libxenon
 * console framebuffer and re-open the console so printf is visible.
 * v0.16: also the "back to the menu" step; gpu_present_init() resumes. */
void gpu_present_finish(void);

void gpu_present_get_stats(gpu_present_stats_t *out);

#endif
