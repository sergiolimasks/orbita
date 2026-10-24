/*
 * XCloud360 — h264play v0.7.0 hybrid TCP live + embedded fallback.
 *
 * Boot flow:
 *   1. stream_init() — se Aurora ja tem netif up, skip network_init, connect TCP
 *   2. Se TCP conecta em <5s: length-prefix AU decode loop
 *   3. Se TCP falha OU eof: fallback pra embedded stream (proven v0.6.0 path)
 *
 * Aurora-safe: nao chama network_init se netif.ip_addr.addr != 0.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/timebase.h>
#include <ppc/atomic.h>
#include <time/time.h>
#include <input/input.h>
#include <usb/usbmain.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include "tcp_stream.h"
#include "input_udp.h"

extern const uint8_t embedded_stream[];
extern const size_t embedded_stream_len;

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.11.2-ringatomic"
#endif

#ifndef XCLOUD360_STREAM_SERVER_IP
#define XCLOUD360_STREAM_SERVER_IP "192.168.15.64"
#endif
#ifndef XCLOUD360_STREAM_SERVER_PORT
#define XCLOUD360_STREAM_SERVER_PORT 9000
#endif

#define FB_W 1280
#define FB_H 720

/* ============================================================
 *  P7.2 — 2-core pipeline
 *  Main thread (core 0):  decode, memcpy planes into ring slot
 *  Worker thread (core 2, primary of core 1): convert+present
 *  2-slot ring so main can produce N+1 while worker consumes N.
 * ============================================================ */
#define RING_SLOTS 2
struct ring_slot {
    uint8_t Y[FB_W * FB_H];            /* 921600 */
    uint8_t U[(FB_W/2) * (FB_H/2)];    /* 230400 */
    uint8_t V[(FB_W/2) * (FB_H/2)];    /* 230400 */
    int W, H;
    volatile int ready;                /* 1 = producer filled; 0 = free */
    volatile int done;                 /* 1 = consumer done (mirror) */
    volatile uint64_t cvt_us;          /* time consumer spent in convert */
#ifdef XCLOUD360_TRACE_LATENCY
    volatile uint64_t au_seq;          /* Fase 0: rastrear qual AU esse slot representa */
#endif
};
static struct ring_slot g_ring[RING_SLOTS] __attribute__((aligned(128)));
static volatile int g_prod_idx = 0;
static volatile int g_cons_idx = 0;
static volatile int g_producer_done = 0;
static volatile uint64_t g_cvt_us_total = 0;
static volatile int g_worker_frames = 0;

/* 64 KB per worker stack, aligned. */
static uint8_t g_worker_stack[65536] __attribute__((aligned(128)));

static uint64_t now_us(void) {
    return mftb() / (PPC_TIMEBASE_FREQ / 1000000ULL);
}

/* ============================================================
 *  Fase 0 — Instrumentação AU→flip. Sob TRACE_LATENCY.
 *  4 timestamps do MESMO relógio (mftb) por AU sequencial:
 *   - t_arrival:  bytes do AU completos no ring
 *   - t_decoded:  avcodec_decode_video2 devolveu frame
 *   - t_dispatch: main thread produziu slot pro worker
 *   - t_flipped:  worker terminou o write no framebuffer
 *  + ring_bytes no instante de t_arrival.
 *  Array cíclico 64 amostras. Main escreve arrival/decoded/dispatch;
 *  worker escreve flipped indexado por au_seq lido do slot.
 * ============================================================ */
#ifdef XCLOUD360_TRACE_LATENCY
#define TRACE_HISTORY 64
typedef struct {
    uint64_t au_seq;
    uint64_t t_arrival;
    uint64_t t_decoded;
    uint64_t t_dispatch;
    volatile uint64_t t_flipped;
    size_t   ring_bytes_at_arrival;
    uint8_t  skipped;   /* main pulou convert dispatch (skip_n) */
    volatile uint8_t  flipped_seen;
} trace_slot_t;
static trace_slot_t g_trace[TRACE_HISTORY] __attribute__((aligned(128)));
static uint64_t g_au_seq_counter = 0;
static uint64_t g_trace_last_reported = 0;   /* último au_seq resumido */
#endif

/* -------- Direct Xenos tiled framebuffer writer (P7.1) -----------------
 * P7.0 used console_pset() per pixel — ~29ms/frame @720p60 = infeasible.
 * P7.1 replicates the exact formula from libxenon/drivers/console/console.c
 * inline and writes 4 pixels per iteration (aligned x). Xenos tile layout:
 * 32x32 tiles, blocks of 4x1 pixels contiguously stored, X-swap on (y&8).
 * fb pointer comes from the ATI info register at 0xec806100.
 * `| 0x80000000` maps to non-cached alias — writes go straight to RAM,
 * no dcbst needed. */
static uint32_t* g_fb = NULL;
static int g_fb_stride_pixels = 0;   /* padded to multiple of 32 */

static uint32_t g_fb_width  = 0;
static uint32_t g_fb_height = 0;
static void fb_direct_init(void) {
    struct ati_info { uint32_t u1[4]; uint32_t base; uint32_t u2[8]; uint32_t width, height; } __attribute__((packed));
    volatile struct ati_info* ai = (struct ati_info*)0xec806100ULL;
    uint32_t raw_base = ai->base;
    uint32_t raw_w    = ai->width;
    uint32_t raw_h    = ai->height;
    printf("[fb-init] ai->base=0x%08x  ai->width=%u  ai->height=%u\n",
           raw_base, raw_w, raw_h);
    if (raw_w == 0 || raw_w > 4096 || raw_h == 0 || raw_h > 4096) {
        printf("[fb-init] BAD ATI info -- direct FB DISABLED, fallback pset\n");
        g_fb = NULL; return;
    }
    g_fb = (uint32_t*)(unsigned long)(raw_base | 0x80000000);
    g_fb_stride_pixels = ((raw_w + 31) >> 5) << 5;
    g_fb_width = raw_w;
    g_fb_height = raw_h;
    printf("[fb-init] fb=%p stride=%u  active=%ux%u  OK\n",
           (void*)g_fb, g_fb_stride_pixels, raw_w, raw_h);
}

/* Compute Xenos tile pixel offset. x must be pixel-aligned (any value works,
 * but the 4-pixel batch below assumes x % 4 == 0). */
static inline uint32_t fb_tile_off(int x, int y) {
    return ((y >> 5) * 32 * g_fb_stride_pixels
          + ((x >> 5) << 10)
          + (x & 3) + ((y & 1) << 2)
          + (((x & 31) >> 2) << 3)
          + (((y & 31) >> 1) << 6))
          ^ ((y & 8) << 2);
}

/* Pack BGRA0 word matching what console_pset does: (b<<24)|(g<<16)|(r<<8). */
static inline uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8);
}

static inline uint8_t clip8(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* BT.601 SD YUV to RGB (video-range 16-235 Y, 16-240 UV).
 * Integer fixed-point Q10. Chroma is 4:2:0 (subsampled 2x in both dims). */

/* P7.1: 4 luma per iter, 2 rows in parallel. Compute base tile offset ONCE
 * per 4-pixel block (aligned to 4 in X), then 4 contiguous 32-bit stores
 * (Xenos 4x1 sub-block). This is the inner loop — hot path budget was
 * ~29ms; goal is <=3ms. */
static void convert_and_present_2rows_fast(
    const uint8_t* Y0, const uint8_t* Y1,
    const uint8_t* U,  const uint8_t* V,
    int W, int y_row)
{
    /* Bounds guard: don't write past active display area. */
    if (!g_fb) return;
    if ((uint32_t)(y_row + 1) >= g_fb_height) return;
    if ((uint32_t)W > g_fb_width) W = g_fb_width;

    for (int x = 0; x < W; x += 4) {
        /* 2 UV samples cover 4 luma horizontally (4:2:0) */
        int u0 = U[(x  )>>1] - 128, v0 = V[(x  )>>1] - 128;
        int u1 = U[(x+2)>>1] - 128, v1 = V[(x+2)>>1] - 128;
        int rc0 = ( 1596 * v0 + 512) >> 10;
        int gc0 = (-391 * u0 - 813 * v0 + 512) >> 10;
        int bc0 = ( 2018 * u0 + 512) >> 10;
        int rc1 = ( 1596 * v1 + 512) >> 10;
        int gc1 = (-391 * u1 - 813 * v1 + 512) >> 10;
        int bc1 = ( 2018 * u1 + 512) >> 10;

        int y00 = 1164 * (Y0[x  ] - 16) >> 10;
        int y01 = 1164 * (Y0[x+1] - 16) >> 10;
        int y02 = 1164 * (Y0[x+2] - 16) >> 10;
        int y03 = 1164 * (Y0[x+3] - 16) >> 10;
        int y10 = 1164 * (Y1[x  ] - 16) >> 10;
        int y11 = 1164 * (Y1[x+1] - 16) >> 10;
        int y12 = 1164 * (Y1[x+2] - 16) >> 10;
        int y13 = 1164 * (Y1[x+3] - 16) >> 10;

        /* 4-pixel block is contiguous in Xenos tile — base+0..3 covers it */
        uint32_t base_r0 = fb_tile_off(x, y_row);
        uint32_t base_r1 = fb_tile_off(x, y_row + 1);
        g_fb[base_r0 + 0] = pack_rgb(clip8(y00 + rc0), clip8(y00 + gc0), clip8(y00 + bc0));
        g_fb[base_r0 + 1] = pack_rgb(clip8(y01 + rc0), clip8(y01 + gc0), clip8(y01 + bc0));
        g_fb[base_r0 + 2] = pack_rgb(clip8(y02 + rc1), clip8(y02 + gc1), clip8(y02 + bc1));
        g_fb[base_r0 + 3] = pack_rgb(clip8(y03 + rc1), clip8(y03 + gc1), clip8(y03 + bc1));
        g_fb[base_r1 + 0] = pack_rgb(clip8(y10 + rc0), clip8(y10 + gc0), clip8(y10 + bc0));
        g_fb[base_r1 + 1] = pack_rgb(clip8(y11 + rc0), clip8(y11 + gc0), clip8(y11 + bc0));
        g_fb[base_r1 + 2] = pack_rgb(clip8(y12 + rc1), clip8(y12 + gc1), clip8(y12 + bc1));
        g_fb[base_r1 + 3] = pack_rgb(clip8(y13 + rc1), clip8(y13 + gc1), clip8(y13 + bc1));
    }
}

/* Worker thread — runs on core 1 primary (thread 2 in PIR terms). */
static void worker_convert(void) {
    while (1) {
        /* Wait for a produced slot */
        int idx = g_cons_idx % RING_SLOTS;
        while (!g_ring[idx].ready) {
            if (g_producer_done) {
                /* Drain: one more check then bail */
                if (!g_ring[idx].ready) return;
                break;
            }
            /* spin-wait; short delay to avoid starving SMT peer */
            for (volatile int i = 0; i < 100; i++) {}
        }

        /* Acquire barrier: ensure plane reads happen AFTER seeing ready=1.
         * lwsync = load-load + load-store barrier (lightweight sync). */
        __asm__ __volatile__("lwsync" ::: "memory");
        uint64_t t0 = now_us();
        int W = g_ring[idx].W;
        int H = g_ring[idx].H;
        const uint8_t* Yp = g_ring[idx].Y;
        const uint8_t* Up = g_ring[idx].U;
        const uint8_t* Vp = g_ring[idx].V;
        for (int y = 0; y < H; y += 2) {
            convert_and_present_2rows_fast(
                Yp + y*W,      Yp + (y+1)*W,
                Up + (y/2)*(W/2), Vp + (y/2)*(W/2),
                W, y);
        }
        uint64_t t1 = now_us();
        g_ring[idx].cvt_us = t1 - t0;
        g_cvt_us_total += (t1 - t0);
        g_worker_frames++;
#ifdef XCLOUD360_TRACE_LATENCY
        /* Registrar t_flipped na célula do au_seq que esse slot representa.
         * lwsync acima já garantiu que au_seq foi lido em ordem com ready=1. */
        uint64_t seq = g_ring[idx].au_seq;
        int tslot = (int)(seq % TRACE_HISTORY);
        g_trace[tslot].t_flipped = t1;
        __asm__ __volatile__("eieio" ::: "memory");
        g_trace[tslot].flipped_seen = 1;
#endif
        g_ring[idx].ready = 0;   /* free the slot */
        g_cons_idx++;
    }
}

/* P11 v0.9.6: axial deadzone (X e Y independentes). Radial 8000 do v0.9.5 não
 * pegou drift assimétrico do stick (Sergio reportou cursor indo pra ESQUERDA
 * — X drift consistente > 8000 mas Y perto de zero, ficava fora do círculo
 * radial). Axial 12000 pega drift em qualquer eixo separadamente. */
#define STICK_DEADZONE 12000
static void apply_deadzone(int16_t *x, int16_t *y) {
    int ix = *x, iy = *y;
    if (ix < 0) { if (-ix < STICK_DEADZONE) *x = 0; }
    else        { if ( ix < STICK_DEADZONE) *x = 0; }
    if (iy < 0) { if (-iy < STICK_DEADZONE) *y = 0; }
    else        { if ( iy < STICK_DEADZONE) *y = 0; }
}
/* Trigger deadzone — analog triggers can rest at 4-8 out of 255. */
#define TRIGGER_DEADZONE 12
static uint8_t apply_trigger_dz(uint8_t t) {
    return t <= TRIGGER_DEADZONE ? 0 : t;
}

/* P11: read USB gamepad, pack into 24-byte little-endian frame, UDP -> devbox:9001. */
static void poll_and_send_gamepad(void) {
    struct controller_data_s d;
    int r = get_controller_data(&d, 0);
    /* r==1 means libxenon just delivered a fresh USB report from slot 0 —
     * proves the wireless controller synced + is being polled. Print once. */
    static int first_valid = 0;
    if (r && !first_valid) {
        printf("[input] controller slot 0 attached — first fresh report\n");
        first_valid = 1;
    }
    /* Y invertido: libxenon Xbox 360 controller devolve Y positivo = stick UP,
     * mas Green-NX serializer JÁ inverte pra Switch (axis(-left_y)). Isso
     * resulta em dupla-inversão errada pro xCloud. Negamos aqui pra
     * cancelar. Sergio confirmou 2026-09-10: UP → cursor DOWN, DOWN → UP.
     * Clamp em INT16_MIN pra evitar overflow em -(-32768). */
    int16_t sx1 = d.s1_x;
    int16_t sy1 = (d.s1_y == -32768) ? 32767 : -d.s1_y;
    int16_t sx2 = d.s2_x;
    int16_t sy2 = (d.s2_y == -32768) ? 32767 : -d.s2_y;
    apply_deadzone(&sx1, &sy1);
    apply_deadzone(&sx2, &sy2);
    uint8_t lt = apply_trigger_dz(d.lt);
    uint8_t rt = apply_trigger_dz(d.rt);
    uint8_t buf[24];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0;
    buf[1] = (uint8_t)(
          (d.a ? 1 : 0)
        | ((d.b ? 1 : 0) << 1)
        | ((d.x ? 1 : 0) << 2)
        | ((d.y ? 1 : 0) << 3)
        | ((d.up ? 1 : 0) << 4)
        | ((d.down ? 1 : 0) << 5)
        | ((d.left ? 1 : 0) << 6)
        | ((d.right ? 1 : 0) << 7));
    buf[2] = (uint8_t)(
          (d.start ? 1 : 0)
        | ((d.back ? 1 : 0) << 1)
        | ((d.logo ? 1 : 0) << 2)
        | ((d.lb ? 1 : 0) << 3)
        | ((d.rb ? 1 : 0) << 4)
        | ((d.s1_z ? 1 : 0) << 5)
        | ((d.s2_z ? 1 : 0) << 6));
    uint16_t ltu = (uint16_t)lt * 257;    /* 0-255 -> 0-65535 */
    uint16_t rtu = (uint16_t)rt * 257;
    buf[3] = (uint8_t)(ltu & 0xff);  buf[4]  = (uint8_t)((ltu >> 8) & 0xff);
    buf[5] = (uint8_t)(rtu & 0xff);  buf[6]  = (uint8_t)((rtu >> 8) & 0xff);
    uint16_t lx = (uint16_t)sx1, ly = (uint16_t)sy1;
    uint16_t rx = (uint16_t)sx2, ry = (uint16_t)sy2;
    buf[7]  = (uint8_t)(lx & 0xff);  buf[8]  = (uint8_t)((lx >> 8) & 0xff);
    buf[9]  = (uint8_t)(ly & 0xff);  buf[10] = (uint8_t)((ly >> 8) & 0xff);
    buf[11] = (uint8_t)(rx & 0xff);  buf[12] = (uint8_t)((rx >> 8) & 0xff);
    buf[13] = (uint8_t)(ry & 0xff);  buf[14] = (uint8_t)((ry >> 8) & 0xff);
    input_udp_send(buf, 24);
    /* Diagnostic: print visible line whenever any button is pressed OR
     * sticks leave the deadzone. Empty otherwise so it does not scroll
     * the console up. Confirms whether libxenon is actually reading the
     * wireless controller after boot. */
    static int prev_any = 0;
    int any = d.a || d.b || d.x || d.y || d.up || d.down || d.left || d.right
            || d.start || d.back || d.logo || d.lb || d.rb || d.s1_z || d.s2_z
            || sx1 || sy1 || sx2 || sy2 || lt > 20 || rt > 20;
    if (any && !prev_any) {
        printf("[pad] A=%d B=%d X=%d Y=%d U/D/L/R=%d%d%d%d Sta/Ba/Lo=%d%d%d LB/RB=%d%d LX=%d LY=%d\n",
               d.a, d.b, d.x, d.y, d.up, d.down, d.left, d.right,
               d.start, d.back, d.logo, d.lb, d.rb, sx1, sy1);
    }
    prev_any = any;
}

/* Length-prefix AU extractor pra TCP stream. Retorna 1 se AU pronta, 0 se pending.
 * v0.11.1: g_au_buf cresceu 128KB → 512KB. Cena 3000kbps 720p complexa gera IDR
 * concentrando 20+ NALs em um AU único; 128KB estourava, disparava stream_reset,
 * ring inteiro perdido, decoder na sopa até próximo IDR. Sintoma: normal→artefato
 * cíclico. Se ainda estourar 512KB, TCP inteiro é derrubado — reconexao busca
 * próximo IDR limpo da xCloud em vez de tentar recuperar meio de bytestream. */
#define AU_BUF_CAP (512 * 1024)
static uint8_t  g_au_buf[AU_BUF_CAP];
static uint32_t g_pending_au_size = 0;

static int pull_next_au(uint8_t **out, int *out_size) {
    if (g_pending_au_size == 0) {
        if (stream_available() < 4) return 0;
        uint8_t hdr[4];
        stream_read(hdr, 4);
        g_pending_au_size = ((uint32_t)hdr[0] << 24) |
                            ((uint32_t)hdr[1] << 16) |
                            ((uint32_t)hdr[2] <<  8) |
                            ((uint32_t)hdr[3]);
        if (g_pending_au_size == 0 || g_pending_au_size > AU_BUF_CAP) {
            /* Fase 1 do plano Sergio: se au_size veio corrompido, não sabemos
             * onde começa o próximo AU. Derrubar conexão TCP força capture-rtp
             * a mandar novo IDR na reconexão — sync limpo garantido. */
            printf("[play] bogus au_size=%u (cap=%d) — DROP TCP for clean re-IDR\n",
                   g_pending_au_size, AU_BUF_CAP);
            g_pending_au_size = 0;
            stream_force_reconnect();
            return 0;
        }
    }
    if (stream_available() < g_pending_au_size) return 0;
    stream_read(g_au_buf, g_pending_au_size);
    *out = g_au_buf;
    *out_size = (int)g_pending_au_size;
    g_pending_au_size = 0;
    return 1;
}

int main(void) {
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);
    xenon_thread_startup();
    usb_init();
    /* Warm-up drain: RF unit + wireless controller take ~500-2000ms to
     * enumerate + sync. Without repeated usb_do_poll(), interrupt reports
     * never arrive, ctrl[0] stays zero-init, and controls read as "all
     * released". 200 iters * 10ms = 2s window. */
    for (int i = 0; i < 200; i++) { usb_do_poll(); udelay(10000); }
    fb_direct_init();

    /* P7.2 worker thread — core 1 primary (thread 2). Stack grows down so
     * pass TOP-address. Leave 128B guard at very top for xenon prologue. */
    for (int i = 0; i < RING_SLOTS; i++) {
        g_ring[i].ready = 0; g_ring[i].done = 0; g_ring[i].cvt_us = 0;
    }
    void *worker_stack_top = g_worker_stack + sizeof(g_worker_stack) - 128;
    int spawn_r = xenon_run_thread_task(2, worker_stack_top, (void*)worker_convert);
    printf("[p7.2] worker spawn thread=2 stack_top=%p r=%d\n",
           worker_stack_top, spawn_r);

    printf("\n\n==== XCloud360 h264play v%s ====\n\n", XCLOUD360_BUILD_VERSION);
    printf("[stream] embedded fallback %zu bytes\n", embedded_stream_len);
    printf("[fb]     direct write @ %p stride=%d px\n", (void*)g_fb, g_fb_stride_pixels);

    /* Try TCP first — Aurora-safe (skip network_init if netif already up). */
    printf("[stream] trying TCP %s:%d...\n",
           XCLOUD360_STREAM_SERVER_IP, XCLOUD360_STREAM_SERVER_PORT);
    int use_tcp = (stream_init(XCLOUD360_STREAM_SERVER_IP, XCLOUD360_STREAM_SERVER_PORT) == 0);
    if (use_tcp) {
        /* Wait up to 5s for first data. */
        printf("[stream] TCP init OK, waiting for data (max 5s)...\n");
        uint64_t deadline = now_us() + 5000000;
        while (now_us() < deadline && stream_available() < 4) {
            stream_poll();
            udelay(10000);
        }
        if (stream_available() < 4) {
            printf("[stream] TCP no data — fallback embedded\n");
            use_tcp = 0;
        } else {
            printf("[stream] TCP live feed active (buf=%zuB)\n", stream_available());
            /* P11: safe to open UDP now — netif is up, stream_client's
             * tcp_active_pcbs settled, no risk of the double-tcp_new bug
             * that killed v0.8.2. */
            int ir = input_udp_init(XCLOUD360_STREAM_SERVER_IP, 9001);
            printf("[input-udp] init r=%d\n", ir);
        }
    } else {
        printf("[stream] TCP init failed — using embedded\n");
    }

    avcodec_register_all();
    AVCodec* codec = avcodec_find_decoder(CODEC_ID_H264);
    if (!codec) { printf("[err] no H264 decoder\n"); goto stop; }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 1;
    ctx->flags  |= CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= CODEC_FLAG2_FAST;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        printf("[err] avcodec_open2 failed\n"); goto stop;
    }
    AVCodecParserContext* parser = av_parser_init(codec->id);
    AVFrame* frame = avcodec_alloc_frame();
    AVPacket pkt;
    av_init_packet(&pkt);

    printf("[play] mode=%s -> decode -> BT.601 -> direct-FB tiled\n\n",
           use_tcp ? "TCP-live" : "embedded");

    uint64_t t_wall_start = now_us();
    uint64_t t_next_report = t_wall_start + 10000000;   /* v0.8.3: 10s cadence — linha estável na tela pra filmar */
    uint64_t acc_dec = 0, acc_cvt = 0;
    int frames_since_report = 0;
    int total_frames = 0;

    const uint8_t *data = embedded_stream;
    size_t left = embedded_stream_len;

    for (;;) {
        uint8_t *out_data = NULL;
        int out_size = 0;
#ifdef XCLOUD360_TRACE_LATENCY
        uint64_t trace_au_seq = 0;   /* válido só se use_tcp — mata warn de escopo */
#endif

        if (use_tcp) {
            stream_poll();
            /* v0.9.8: drop-on-pressure REMOVIDO. Cortava bytestream no meio de
             * AU → libavcodec "error while decoding MB X Y, bytestream (t.)"
             * → sopa de artefatos até próxima IDR. Solução: só variar taxa de
             * convert do worker (skip adaptativo, ver mais abaixo). Decoder
             * continua decodando bytestream inteiro sempre. */
            int pulled = pull_next_au(&out_data, &out_size);
            if (pulled <= 0) {
                usb_do_poll();
                poll_and_send_gamepad();   /* P11: input pump idle path */
                udelay(1000);
                continue;
            }
#ifdef XCLOUD360_TRACE_LATENCY
            /* Fase 0: AU completo tá no ring. Reservar slot no anel de trace. */
            trace_au_seq = g_au_seq_counter++;
            int tslot_init = (int)(trace_au_seq % TRACE_HISTORY);
            g_trace[tslot_init].au_seq = trace_au_seq;
            g_trace[tslot_init].t_arrival = now_us();
            g_trace[tslot_init].ring_bytes_at_arrival = stream_available();
            g_trace[tslot_init].t_decoded = 0;
            g_trace[tslot_init].t_dispatch = 0;
            g_trace[tslot_init].t_flipped = 0;
            g_trace[tslot_init].skipped = 0;
            g_trace[tslot_init].flipped_seen = 0;
#endif
            usb_do_poll();                 /* P11: drain USB reports even */
            poll_and_send_gamepad();       /* on active path or wireless */
        } else {
            if (left == 0) break;
            int used = av_parser_parse2(parser, ctx,
                &out_data, &out_size, (uint8_t*)data, left,
                AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) break;
            data += used; left -= used;
            if (out_size <= 0) continue;
        }

        pkt.data = out_data; pkt.size = out_size;
        uint64_t t_a = now_us();
        int got = 0;
        int r = avcodec_decode_video2(ctx, frame, &got, &pkt);
        uint64_t t_b = now_us();
        if (r < 0) {
            /* Non-fatal: decoder rejected AU (missing reference picture after
             * IDR loss, corrupt slice, etc). Skip and wait for next IDR
             * instead of tearing down the whole session. */
            if (use_tcp) continue;
            break;   /* embedded stream: break on decode error */
        }
        if (!got) continue;
#ifdef XCLOUD360_TRACE_LATENCY
        /* Fase 0: decode retornou got=1 pra esse AU. */
        if (use_tcp) g_trace[(int)(trace_au_seq % TRACE_HISTORY)].t_decoded = t_b;
#endif

        /* Fase 2b (v0.11.0): descarte na SAIDA, não backpressure na entrada.
         * Se o worker ainda não drenou o slot anterior, o frame decodado atual
         * é descartado — o próximo decode continua imediatamente sem esperar.
         * Regra: fila máxima = 1 frame em flight visual. Decoder mantém P-frame
         * reference chain sempre (não pula decode, só pula dispatch pro worker). */
        static int decoded_count = 0;
        decoded_count++;
        acc_dec += (t_b - t_a);
        total_frames++;

        int prod = g_prod_idx % RING_SLOTS;
        if (g_ring[prod].ready) {
#ifdef XCLOUD360_TRACE_LATENCY
            if (use_tcp) g_trace[(int)(trace_au_seq % TRACE_HISTORY)].skipped = 1;
#endif
            continue;   /* worker ocupado — drop este frame na saída */
        }

        int W = frame->width  < FB_W ? frame->width  : FB_W;
        int H = frame->height < FB_H ? frame->height : FB_H;
        int Yst  = frame->linesize[0];
        int UVst = frame->linesize[1];
        const uint8_t* Yp = frame->data[0];
        const uint8_t* Up = frame->data[1];
        const uint8_t* Vp = frame->data[2];

        /* Slot livre — publica pro worker. Sem while() de espera. */
        uint64_t t_c = now_us();
        g_ring[prod].W = W;
        g_ring[prod].H = H;
        for (int y = 0; y < H; y++) {
            memcpy(g_ring[prod].Y + y*W, Yp + y*Yst, W);
        }
        for (int y = 0; y < H/2; y++) {
            memcpy(g_ring[prod].U + y*(W/2), Up + y*UVst, W/2);
            memcpy(g_ring[prod].V + y*(W/2), Vp + y*UVst, W/2);
        }
#ifdef XCLOUD360_TRACE_LATENCY
        if (use_tcp) {
            g_ring[prod].au_seq = trace_au_seq;
            g_trace[(int)(trace_au_seq % TRACE_HISTORY)].t_dispatch = now_us();
        }
#endif
        __asm__ __volatile__("eieio; sync" ::: "memory");
        g_ring[prod].ready = 1;
        g_prod_idx++;
        uint64_t t_d = now_us();

        /* v0.9.6: acc_dec / total_frames já contabilizados antes do skip. */
        acc_cvt += (t_d - t_c);       /* now = memcpy + backpressure wait */
        frames_since_report++;

        if (t_d >= t_next_report) {
            double sec = (t_d - (t_next_report - 10000000)) / 1e6;
            double dec_ms = (double)acc_dec / 1000.0 / frames_since_report;
            double prod_ms = (double)acc_cvt / 1000.0 / frames_since_report;
            double fps    = frames_since_report / sec;
            int worker_fps_snapshot = g_worker_frames;
            double wcvt_ms = worker_fps_snapshot > 0
                ? (double)g_cvt_us_total / 1000.0 / worker_fps_snapshot : 0;
            size_t ring_bytes = stream_available();
            printf("[play] t+%3ds dec=%.1f prod=%.1f w_cvt=%.1f fps=%.1f wF=%d ring=%zu\n",
                (int)((t_d - t_wall_start)/1000000),
                dec_ms, prod_ms, wcvt_ms, fps, worker_fps_snapshot, ring_bytes);
            acc_dec = acc_cvt = 0; frames_since_report = 0;
            t_next_report = t_d + 10000000;   /* 10s */
#ifdef XCLOUD360_TRACE_LATENCY
            /* Fase 0: sumario das amostras com t_flipped preenchido desde o
             * ultimo report. Trace_last_reported é o proximo au_seq esperado. */
            uint64_t stop_seq = g_au_seq_counter;
            uint64_t d_ar_de_min = ~0ULL, d_ar_de_max = 0, d_ar_de_sum = 0;
            uint64_t d_de_di_min = ~0ULL, d_de_di_max = 0, d_de_di_sum = 0;
            uint64_t d_di_fl_min = ~0ULL, d_di_fl_max = 0, d_di_fl_sum = 0;
            uint64_t d_ar_fl_min = ~0ULL, d_ar_fl_max = 0, d_ar_fl_sum = 0;
            size_t   ring_min = ~0U, ring_max = 0; uint64_t ring_sum = 0;
            int n_full = 0, n_skipped = 0, n_pending = 0;
            for (uint64_t s = g_trace_last_reported; s < stop_seq; s++) {
                int idx = (int)(s % TRACE_HISTORY);
                if (g_trace[idx].au_seq != s) continue;  /* recycled beyond window */
                uint64_t ta = g_trace[idx].t_arrival;
                uint64_t td = g_trace[idx].t_decoded;
                uint64_t tp = g_trace[idx].t_dispatch;
                uint64_t tf = g_trace[idx].t_flipped;
                if (g_trace[idx].skipped) { n_skipped++; continue; }
                if (!tf) { n_pending++; continue; }
                n_full++;
                uint64_t d1 = td - ta;
                uint64_t d2 = tp - td;
                uint64_t d3 = tf - tp;
                uint64_t d4 = tf - ta;
                if (d1 < d_ar_de_min) d_ar_de_min = d1;
                if (d1 > d_ar_de_max) d_ar_de_max = d1;
                d_ar_de_sum += d1;
                if (d2 < d_de_di_min) d_de_di_min = d2;
                if (d2 > d_de_di_max) d_de_di_max = d2;
                d_de_di_sum += d2;
                if (d3 < d_di_fl_min) d_di_fl_min = d3;
                if (d3 > d_di_fl_max) d_di_fl_max = d3;
                d_di_fl_sum += d3;
                if (d4 < d_ar_fl_min) d_ar_fl_min = d4;
                if (d4 > d_ar_fl_max) d_ar_fl_max = d4;
                d_ar_fl_sum += d4;
                size_t rb = g_trace[idx].ring_bytes_at_arrival;
                if (rb < ring_min) ring_min = rb;
                if (rb > ring_max) ring_max = rb;
                ring_sum += rb;
            }
            if (n_full > 0) {
                printf("[trace] n=%d skip=%d pend=%d | arr->dec avg=%llu max=%llu | dec->disp avg=%llu max=%llu | disp->flip avg=%llu max=%llu | arr->flip avg=%llu max=%llu us | ring avg=%zu max=%zu\n",
                    n_full, n_skipped, n_pending,
                    (unsigned long long)(d_ar_de_sum / n_full), (unsigned long long)d_ar_de_max,
                    (unsigned long long)(d_de_di_sum / n_full), (unsigned long long)d_de_di_max,
                    (unsigned long long)(d_di_fl_sum / n_full), (unsigned long long)d_di_fl_max,
                    (unsigned long long)(d_ar_fl_sum / n_full), (unsigned long long)d_ar_fl_max,
                    (size_t)(ring_sum / n_full), ring_max);
            } else {
                printf("[trace] no full samples (skipped=%d pending=%d)\n", n_skipped, n_pending);
            }
            g_trace_last_reported = stop_seq;
#endif
        }
    }
    /* Signal worker to drain and exit */
    g_producer_done = 1;
    while (g_worker_frames < total_frames && xenon_is_thread_task_running(2)) {
        for (volatile int i = 0; i < 10000; i++) {}
    }

    uint64_t t_wall_end = now_us();
    double wall_s = (t_wall_end - t_wall_start) / 1e6;
    double fps = total_frames / wall_s;
    printf("\n==== RESULT ====\n");
    printf("frames         : %d\n", total_frames);
    printf("wall time      : %.2f s\n", wall_s);
    printf("effective fps  : %.2f\n", fps);
    if (fps >= 55.0)      printf("verdict        : PASS\n");
    else if (fps >= 30.0) printf("verdict        : PARTIAL\n");
    else                  printf("verdict        : FAIL\n");

stop:
    printf("\n[end] holding — power-cycle to return\n");
    for (;;) {
        usb_do_poll();
        udelay(50000);
    }
    return 0;
}
