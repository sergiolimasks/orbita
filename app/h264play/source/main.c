/*
 * XCloud360 — h264play v0.7.0 hybrid TCP live + embedded fallback.
 *
 * Boot flow:
 *   1. stream_init() — se Aurora ja tem netif up, skip network_init, connect TCP
 *   2. Se TCP conecta em <5s: length-prefix AU decode loop
 *   3. Se TCP falha OU eof: fallback pra embedded stream (proven v0.6.0 path)
 *
 * Aurora-safe: nao chama network_init se netif.ip_addr.addr != 0.
 *
 * v0.16 (make MENU=1, padrão): boota num menu de jogos em vez de ir direto
 * pro stream. Conexão de controle com o launcher do PC (:9002, ctrl_conn.c),
 * lista de jogos no console (menu_ui.c/menu_model.c), PLAY -> o vídeo chega
 * na :9000 como antes -> pipeline de vídeo (run_video). Back+Start 2s no
 * jogo = STOP e volta pro menu. MENU=0 = fluxo antigo acima, intacto.
 *
 * v0.17 (make XBOX_LOGIN=1, padrão): o próprio Xbox faz o login Microsoft/
 * Xbox e baixa o catálogo xCloud por HTTPS (source/login/, thread hw 3);
 * o PLAY continua indo pro launcher do PC. Falhou = catálogo do launcher
 * como na v0.16, com o passo que falhou na tela. XBOX_LOGIN=0 = v0.16.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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
#ifdef XCLOUD360_GPU
#include "gpu_present.h"   /* v0.15: YUV->RGB no Xenos (make GPU=1) */
#endif
#ifdef XCLOUD360_MENU
#include "ctrl_conn.h"     /* v0.16: controle com o launcher do PC (:9002) */
#include "menu_model.h"
#include "menu_ui.h"
#endif
#ifdef XCLOUD360_LOGIN
#include "login/login_task.h"   /* v0.17: login + catálogo no Xbox */
#include "login/net_pipe.h"
#endif
#ifdef XCLOUD360_NATIVE
#include "native/xc_native.h"   /* stage 5: sessão + WebRTC no próprio Xbox */
#include "webrtc/xc_udp_xenon.h"
#include "login/xc_port.h"
#endif
#ifdef XCLOUD360_AUDIO
#include "audio/audio.h"        /* v0.18: áudio do jogo (Opus) */
/* v0.18: o teste offline (X) dura pelo menos 2 voltas do tom embutido. */
#define AUDIO_TEST_US 9000000ULL
/* v0.18.0: a 1a build pra hardware deu tela vermelha no boot, antes da rede.
 * Cada passo do boot vai pra tela: a foto mostra o último que passou. */
#define BOOT_STEP(s) printf("[boot] %s\n", s)
#else
#define BOOT_STEP(s) ((void)0)
#endif

extern const uint8_t embedded_stream[];
extern const size_t embedded_stream_len;

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.15.0"
#endif
#ifdef XCLOUD360_NATIVE
#ifndef XST_HW_THREAD
#define XST_HW_THREAD 3
#endif
#ifndef XCLOUD360_NATIVE_DEC_THREADS
#define XCLOUD360_NATIVE_DEC_THREADS 4   /* v0.19.2: hw 4,2,5,3 (sessão na hw1) */
#endif
#define XC360_STR_(x) #x
#define XC360_STR(x) XC360_STR_(x)
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
    volatile int bottom_done;          /* v0.14.4: metade de baixo convertida */
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

/* v0.14.4: convert dividido em duas metades. O worker (hw thread 2, núcleo 1)
 * faz a de cima; a thread de rede (hw thread 1, núcleo 0 — irmã do main, que
 * passa a maior parte do tempo esperando o decoder) faz a de baixo entre um
 * network_poll e outro, via convert_bottom_step(). Telemetria da v0.14.2:
 * decode a 59fps com ring vazio, mas só ~15fps na tela com convert ~64ms. */
static volatile int g_split_active = 0;
static int g_bot_cons_idx = 0;            /* só a thread de rede */
static volatile uint64_t g_bot_us_total = 0;

static inline int split_row(int H) { return (H / 2) & ~1; }

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
#ifdef XCLOUD360_GPU
/* v0.15: com GPU o "flip" é a troca de scanout feita pelo main em
 * gpu_present_poll(), que chama isto com o au_seq passado no submit. */
static void trace_on_gpu_flip(uint64_t seq, uint64_t t_flip_us) {
    int tslot = (int)(seq % TRACE_HISTORY);
    if (g_trace[tslot].au_seq != seq) return;
    g_trace[tslot].t_flipped = t_flip_us;
    g_trace[tslot].flipped_seen = 1;
}
#endif
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
        int split = g_split_active;
        int y_end = split ? split_row(H) : H;
        for (int y = 0; y < y_end; y += 2) {
            convert_and_present_2rows_fast(
                Yp + y*W,      Yp + (y+1)*W,
                Up + (y/2)*(W/2), Vp + (y/2)*(W/2),
                W, y);
        }
        if (split) {
            /* Espera a thread de rede terminar a metade de baixo deste slot. */
            while (!g_ring[idx].bottom_done) {
                if (g_producer_done) return;
                __asm__ __volatile__("db16cyc" ::: "memory");
            }
            __asm__ __volatile__("lwsync" ::: "memory");
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
        /* Ordem importa: ready=0 antes de bottom_done=0. A thread de rede lê
         * bottom_done primeiro e ready depois, então nunca vê bottom_done=0
         * novo com ready=1 velho (e reconverteria o frame antigo). */
        __asm__ __volatile__("sync" ::: "memory");
        g_ring[idx].bottom_done = 0;
        g_cons_idx++;
    }
}

/* v0.14.4: hook da thread de rede. Converte a metade de baixo do próximo
 * slot se ele estiver pronto. Retorna 1 se fez trabalho. */
static int convert_bottom_step(void) {
    if (!g_split_active) return 0;
    int idx = g_bot_cons_idx % RING_SLOTS;
    if (g_ring[idx].bottom_done) return 0;      /* worker ainda não liberou */
    __asm__ __volatile__("lwsync" ::: "memory");
    if (!g_ring[idx].ready) return 0;
    __asm__ __volatile__("lwsync" ::: "memory");
    uint64_t t0 = now_us();
    int W = g_ring[idx].W;
    int H = g_ring[idx].H;
    const uint8_t* Yp = g_ring[idx].Y;
    const uint8_t* Up = g_ring[idx].U;
    const uint8_t* Vp = g_ring[idx].V;
    for (int y = split_row(H); y < H; y += 2) {
        convert_and_present_2rows_fast(
            Yp + y*W,      Yp + (y+1)*W,
            Up + (y/2)*(W/2), Vp + (y/2)*(W/2),
            W, y);
    }
    g_bot_us_total += now_us() - t0;
    __asm__ __volatile__("sync" ::: "memory");
    g_ring[idx].bottom_done = 1;
    g_bot_cons_idx++;
    return 1;
}

/* P11 v0.9.6: radial 8000 do v0.9.5 não pegou drift assimétrico do stick
 * (cursor indo pra ESQUERDA — X drift consistente > 8000 com Y ~0), por isso
 * o limiar de 12000.
 * v0.15.1: radial COM RESCALA. O v0.9.6 zerava cada eixo abaixo de 12000 e
 * passava o valor cru acima disso: o stick saltava de 0 pra 37% de força de
 * uma vez (e o jogo ainda aplica a deadzone dele por cima) → movimento
 * "bruto", passando do ponto. Agora a magnitude abaixo de STICK_DZ vira 0 e
 * acima cresce contínua de 0 até 100%, saturando em STICK_OUTER (sticks
 * gastos não chegam a 32767). Mesmo limiar de 12000 → drift continua morto. */
#ifndef STICK_DZ
#define STICK_DZ 12000
#endif
#ifndef STICK_OUTER
#define STICK_OUTER 31000
#endif
static void apply_deadzone(int16_t *x, int16_t *y, float dz) {
    float fx = *x, fy = *y;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag <= dz) { *x = 0; *y = 0; return; }
    float m = mag > STICK_OUTER ? STICK_OUTER : mag;
    float k = (m - dz) / (STICK_OUTER - dz) * 32767.0f / mag;
    float ox = fx * k, oy = fy * k;
    if (ox > 32767.0f) ox = 32767.0f; else if (ox < -32767.0f) ox = -32767.0f;
    if (oy > 32767.0f) oy = 32767.0f; else if (oy < -32767.0f) oy = -32767.0f;
    *x = (int16_t)ox;
    *y = (int16_t)oy;
}
/* v0.15.3: stick de câmera (direito). Calibrado como o esquerdo ficava
 * "bruto". Curva sobre a magnitude normalizada m (0..1) depois da deadzone:
 *   m' = ADZ + (GAIN - ADZ) * m^EXPO      (m > 0)
 * ADZ ("anti-deadzone") começa a saída logo acima da deadzone do próprio
 * jogo (~20-25%) — sem isso, GAIN<1 e curva empurravam o começo do giro pra
 * ~2/3 do curso físico. GAIN = velocidade máxima (70% da de antes), EXPO =
 * quão devagar cresce no meio. Deadzone própria menor (o drift era só no
 * stick esquerdo). */
#ifndef RSTICK_DZ
#define RSTICK_DZ 9000
#endif
#ifndef RSTICK_ADZ
#define RSTICK_ADZ 0.20f
#endif
#ifndef RSTICK_GAIN
#define RSTICK_GAIN 0.70f
#endif
#ifndef RSTICK_EXPO
#define RSTICK_EXPO 1.5f
#endif
static void apply_curve(int16_t *x, int16_t *y) {
    float fx = *x, fy = *y;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag < 1.0f) return;
    float m = mag / 32767.0f;
    if (m > 1.0f) m = 1.0f;
    float out = RSTICK_ADZ + (RSTICK_GAIN - RSTICK_ADZ) * powf(m, RSTICK_EXPO);
    float k = out * 32767.0f / mag;
    *x = (int16_t)(fx * k);
    *y = (int16_t)(fy * k);
}

/* Trigger deadzone — analog triggers can rest at 4-8 out of 255. */
#define TRIGGER_DEADZONE 12
static uint8_t apply_trigger_dz(uint8_t t) {
    return t <= TRIGGER_DEADZONE ? 0 : t;
}

/* stage 5: 1 = o jogo pedido/em andamento é a sessão nativa (WebRTC no
 * Xbox): controle, KFRQ, STAT e STOP vão pro xc_native, não pro PC. Main
 * escreve (PLAY/fim do jogo); a thread de rede só lê. */
#ifdef XCLOUD360_NATIVE
static volatile int g_native_game = 0;
static volatile int g_pad_neutral_req = 0;
#endif

/* P11: read USB gamepad, pack into 24-byte little-endian frame, UDP -> devbox:9001.
 * v0.19: um frame por controle (até 4); buf[0] = slot do libxenon, que o
 * capture-rtp usa como gamepadIndex do xCloud. */
#define MAX_PADS 4
static void send_pad(int port) {
    struct controller_data_s d;
    int r = get_controller_data(&d, port);
    /* Slot 0 sempre manda (como antes, frame zerado até o controle sincar).
     * Slots 1-3 só depois do primeiro report: senão o PC anunciaria 4
     * controles fantasmas pro jogo. */
    static int seen[MAX_PADS];
    if (r && !seen[port]) {
        printf("[input] controller slot %d attached — first fresh report\n", port);
        seen[port] = 1;
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
    apply_deadzone(&sx1, &sy1, STICK_DZ);
    apply_deadzone(&sx2, &sy2, RSTICK_DZ);
    apply_curve(&sx2, &sy2);       /* v0.15.2: câmera mais lenta/fina */
    uint8_t lt = apply_trigger_dz(d.lt);
    uint8_t rt = apply_trigger_dz(d.rt);
    uint8_t buf[24];
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)port;
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
    /* v0.18.2: o índice do xCloud (buf[0]) é a ordem em que cada controle
     * foi usado pela primeira vez, não o slot do libxenon: o receptor sem
     * fio do 360 ocupa o slot 0 mesmo sem controle pareado, e o controle do
     * One caía no slot 1 = jogador 2 (o jogo ignorava). Controle nunca
     * usado não manda nada; o PC mantém o jogador 1 vivo sozinho. */
    {
        static int xidx[MAX_PADS] = { -1, -1, -1, -1 };
        static int next_idx = 0;
        int act = d.a || d.b || d.x || d.y || d.up || d.down || d.left || d.right
               || d.start || d.back || d.logo || d.lb || d.rb || d.s1_z || d.s2_z
               || sx1 || sy1 || sx2 || sy2 || lt || rt;
        if (xidx[port] < 0) {
            if (!act || next_idx >= MAX_PADS) return;
            xidx[port] = next_idx++;
            printf("[input] slot %d -> jogador %d\n", port, xidx[port] + 1);
        }
        buf[0] = (uint8_t)xidx[port];
    }
#ifdef XCLOUD360_NATIVE
    /* stage 5: mesmo frame de 24 bytes, direto pro xcw_set_gamepad (esta é
     * a thread de rede, a mesma do WebRTC). O xcw anuncia o controle novo
     * com gamepadChanged, como o input_send_loop do capture-rtp. */
    if (g_native_game) xn_pad_frame(buf, 24);
    else
#endif
    input_udp_send(buf, 24);
    /* Diagnostic: print visible line whenever any button is pressed OR
     * sticks leave the deadzone. Empty otherwise so it does not scroll
     * the console up. Confirms whether libxenon is actually reading the
     * wireless controller after boot. */
    static int prev_any_v[MAX_PADS];
    int prev_any = prev_any_v[port];
    int any = d.a || d.b || d.x || d.y || d.up || d.down || d.left || d.right
            || d.start || d.back || d.logo || d.lb || d.rb || d.s1_z || d.s2_z
            || sx1 || sy1 || sx2 || sy2 || lt > 20 || rt > 20;
    if (any && !prev_any) {
        printf("[pad%d] A=%d B=%d X=%d Y=%d U/D/L/R=%d%d%d%d Sta/Ba/Lo=%d%d%d LB/RB=%d%d LX=%d LY=%d\n",
               port, d.a, d.b, d.x, d.y, d.up, d.down, d.left, d.right,
               d.start, d.back, d.logo, d.lb, d.rb, sx1, sy1);
    }
    prev_any_v[port] = any;
}

static void poll_and_send_gamepad(void) {
    for (int port = 0; port < MAX_PADS; port++) send_pad(port);
}

/* v0.18.2: junta os 4 slots (botões em OR, stick com o maior desvio) pro
 * menu e pro Back+Start: qualquer controle navega, inclusive o do One. */
static void get_any_pad(struct controller_data_s *o) {
    struct controller_data_s d;
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < MAX_PADS; i++) {
        get_controller_data(&d, i);
        o->a |= d.a; o->b |= d.b; o->x |= d.x; o->y |= d.y;
        o->up |= d.up; o->down |= d.down; o->left |= d.left; o->right |= d.right;
        o->start |= d.start; o->back |= d.back; o->logo |= d.logo;
        o->lb |= d.lb; o->rb |= d.rb; o->s1_z |= d.s1_z; o->s2_z |= d.s2_z;
        if (d.lt > o->lt) o->lt = d.lt;
        if (d.rt > o->rt) o->rt = d.rt;
        if (abs(d.s1_x) > abs(o->s1_x)) o->s1_x = d.s1_x;
        if (abs(d.s1_y) > abs(o->s1_y)) o->s1_y = d.s1_y;
        if (abs(d.s2_x) > abs(o->s2_x)) o->s2_x = d.s2_x;
        if (abs(d.s2_y) > abs(o->s2_y)) o->s2_y = d.s2_y;
    }
}

/* v0.15.2: leitura do controle na thread de rede, a 1kHz. Antes o main lia
 * o USB uma vez por volta do loop — preso no avcodec, o estado do stick podia
 * esperar ~16ms pra sair. Quando g_input_on_net=1 o main não toca mais no
 * USB (libxenon usb não é thread-safe): só esta thread chama usb_do_poll. */
static volatile int g_input_on_net = 0;
/* v0.16: 0 = lê o USB mas não manda o controle pro jogo (menu: o A que
 * escolhe o jogo não pode chegar no capture-rtp). Sem menu fica sempre 1. */
static volatile int g_pad_forward = 1;
#ifdef XCLOUD360_LOGIN
/* v0.17: a thread do login usa o pendrive (FatFs). USB não é thread-safe:
 * enquanto ela segura, a thread de rede não chama usb_do_poll. */
static volatile int g_usb_hold_req = 0, g_usb_hold_ack = 0;
int xc_usb_hold(int on) {
    if (!on) {
        __asm__ __volatile__("sync" ::: "memory");
        g_usb_hold_req = 0;
        __asm__ __volatile__("sync" ::: "memory");
        /* espera a rede ver a soltura: senão um hold logo em seguida
         * poderia ler o ack velho com a rede entrando no usb_do_poll */
        for (int i = 0; i < 2000 && g_input_on_net && g_usb_hold_ack; i++) udelay(100);
        return 0;
    }
    g_usb_hold_req = 1;
    __asm__ __volatile__("sync" ::: "memory");
    if (!g_input_on_net) return 0;   /* sem thread de rede: o login nem roda */
    /* v0.18.2: antes esperava 200ms e seguia mexendo no USB mesmo sem o ack,
     * com a rede ainda dentro do usb_do_poll (dois threads no USB). Agora
     * espera até 2s e, sem ack, desiste: quem chamou pula o pendrive. */
    for (int i = 0; i < 20000 && !g_usb_hold_ack; i++) udelay(100);
    if (g_usb_hold_ack) return 0;
    g_usb_hold_req = 0;
    __asm__ __volatile__("sync" ::: "memory");
    return -1;
}
#endif
/* v0.17.1: hora do último usb_do_poll (net thread) e o maior buraco entre
 * dois polls desde a última leitura. Controle sem poll recente = estado
 * velho: o menu trata como solto (senão a repetição de tecla dispara sozinha
 * e a lista "pula" 20 itens) e o STAT mostra o buraco (ugap_ms). */
static volatile uint64_t g_usb_poll_us = 0;
static volatile uint32_t g_ugap_max_us = 0, g_ugaps = 0;
#define UGAP_SLOW_US 25000
static int net_hook(void) {
    int worked = g_split_active ? convert_bottom_step() : 0;
#ifdef XCLOUD360_AUDIO
    {
        /* v0.18: jitter buffer + Opus + xenon_sound nesta thread, 1x por ms
         * (o hook gira sem parar na pausa da rede e o nível do DAC é MMIO).
         * Decodificou = volta pro network_poll. */
        static uint64_t last_a = 0;
        uint64_t ta = now_us();
        if (ta - last_a >= 1000) {
            last_a = ta;
            worked |= audio_service();
        }
    }
#endif
#ifdef XCLOUD360_LOGIN
    if (g_usb_hold_req) {
        g_usb_hold_ack = 1;
        __asm__ __volatile__("sync" ::: "memory");
        return worked;
    }
    if (g_usb_hold_ack) { g_usb_hold_ack = 0; __asm__ __volatile__("sync" ::: "memory"); }
#endif
    if (g_input_on_net) {
        static uint64_t last_us = 0;
        uint64_t t = now_us();
        if (t - last_us >= 1000) {
            last_us = t;
            usb_do_poll();
            uint64_t prev = g_usb_poll_us;
            g_usb_poll_us = t;
            if (prev) {
                uint32_t gap = (uint32_t)(t - prev);
                if (gap > g_ugap_max_us) g_ugap_max_us = gap;
                if (gap > UGAP_SLOW_US) g_ugaps++;
            }
            if (g_pad_forward) poll_and_send_gamepad();
        }
    }
#ifdef XCLOUD360_NATIVE
    if (g_pad_neutral_req) {   /* saindo do jogo: solta tudo no xCloud */
        g_pad_neutral_req = 0;
        xn_pad_neutral();
    }
#endif
    return worked;
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

/* v0.12.0 (Fase 1 latência): esperar IDR em vez de decodar lixo.
 * Duas situações entram em g_wait_idr:
 *  - avcodec_decode_video2 < 0 (referência quebrada): antes dava `continue`
 *    e seguia decodando P-frames contra a referência corrompida → sopa até
 *    o próximo IDR periódico.
 *  - backlog: ring com mais de BACKLOG_BYTES pendentes = decoder atrás do
 *    stream. Decodar tudo isso só mostra o passado atrasado (é o "delay do
 *    controle"). Joga fora até o próximo IDR, que zera as referências.
 * Nos dois casos pede IDR pro capture-rtp via UDP "KFRQ" (o PC limita a 1/s). */
#define BACKLOG_BYTES   (128 * 1024)
#define KFRQ_PERIOD_US  500000
static int      g_wait_idr = 0;
static uint64_t g_last_kfrq_us = 0;
static unsigned g_skipped_aus = 0;
static unsigned g_backlog_events = 0;
static unsigned g_decode_errors = 0;

static void request_idr(void) {
    uint64_t now = now_us();
    if (now - g_last_kfrq_us < KFRQ_PERIOD_US) return;
    g_last_kfrq_us = now;
#ifdef XCLOUD360_NATIVE
    /* stage 5: = KFRQ do capture-rtp: PLI + videoKeyframeRequested (o xcw
     * limita a 1/s, como o request_keyframe_throttled) */
    if (g_native_game) { xn_request_keyframe(0); return; }
#endif
    input_udp_send((const uint8_t *)"KFRQ", 4);
}

#ifdef XCLOUD360_NATIVE
/* v0.19.2: a hw 3 (XST_HW_THREAD) é do decoder só com o aluguel na mão
 * (xn_dec_claim_hw3). k_slots do deps/xenon-pthread: GPU = 4,2,5,3,1 -> a
 * 4a thread de frame cai na hw 3; CPU = 4,1,5,3 (hw1 = rede) -> a 3a. */
#ifdef XCLOUD360_GPU
#define XC_DEC_HW3_AT 4
#else
#define XC_DEC_HW3_AT 3
#endif
static int g_dec_hw3 = 0;   /* main: o decoder aberto está com a hw 3 */

/* Mesmo setup do run_video (frame threading, FAST, deblock). */
static AVCodecContext *dec_open_threads(AVCodec *codec, int threads) {
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;
    ctx->thread_count = threads;
    ctx->thread_type  = FF_THREAD_FRAME;
    ctx->flags2 |= CODEC_FLAG2_FAST;
#if XCLOUD360_SKIP_DEBLOCK
    ctx->skip_loop_filter = AVDISCARD_ALL;
#endif
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        av_free(ctx);
        return NULL;
    }
    return ctx;
}

/* No meio do jogo nativo: fecha o decoder (junta as threads de frame) e
 * reabre com `want`, esperando o próximo IDR (pede um). ~1 IDR de vídeo
 * congelado; só acontece quando a sessão precisa da hw 3 (raro) e quando
 * ela devolve. NULL = não reabriu nem com 1 thread (sai do jogo). */
static AVCodecContext *dec_reopen(AVCodec *codec, AVCodecContext *old, int want) {
    int had = old->thread_count;
    uint64_t t0 = now_us();
    avcodec_close(old);
    av_free(old);
    if (g_dec_hw3) { xn_dec_release_hw3(); g_dec_hw3 = 0; }
    if (want >= XC_DEC_HW3_AT) {
        if (xn_dec_claim_hw3()) g_dec_hw3 = 1;
        else want = XC_DEC_HW3_AT - 1;
    }
    AVCodecContext *ctx = NULL;
    for (int t = want; t >= 1 && !ctx; t--) {
        if (g_dec_hw3 && t < XC_DEC_HW3_AT) { xn_dec_release_hw3(); g_dec_hw3 = 0; }
        ctx = dec_open_threads(codec, t);
    }
    if (!ctx && g_dec_hw3) { xn_dec_release_hw3(); g_dec_hw3 = 0; }
    g_wait_idr = 1;
    g_last_kfrq_us = now_us();
    xn_request_keyframe(1);   /* decoder novo: IDR já (sem o limite de 1/s) */
    char l[128];
    snprintf(l, sizeof(l), "[dec] jogo nativo: threads %d -> %d (%s), reaberto em %u ms, esperando IDR", had,
             ctx ? ctx->thread_count : 0, want < had ? "hw3 emprestada pra sessao" : "hw3 de volta",
             (unsigned)((now_us() - t0) / 1000));
    np_log_line(l);
    printf("%s\n", l);
    xn_set_dec_threads(ctx ? ctx->thread_count : 0);
    return ctx;
}
#endif

/* Annex-B scan: 1 se o AU tem slice IDR (NAL type 5). */
static int au_has_idr(const uint8_t *p, int n) {
    for (int i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
            if ((p[i+3] & 0x1f) == 5) return 1;
            i += 2;
        }
    }
    return 0;
}

static int pull_next_au(uint8_t **out, int *out_size) {
    /* v0.14: conexão nova = ring descartado; um header de AU lido pela metade
     * da conexão velha não vale mais (antes ficava pendurado e o próximo
     * "AU" saía desalinhado). O capture-rtp abre toda conexão com IDR. */
    if (stream_take_reset()) {
        g_pending_au_size = 0;
        g_wait_idr = 1;
    }
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

#ifdef XCLOUD360_MENU
/* ============================================================
 *  v0.16 — menu de jogos (make MENU=1)
 *
 *  Donos (quem pode tocar em quê):
 *   - lwIP: só a thread de rede (hw 1), que sobe logo depois do
 *     network_init. O vídeo (:9000) e o controle (:9002, ctrl_conn.c) são
 *     dois pcbs no mesmo net_step; o main só usa APIs com lock/ring.
 *   - USB: só a thread de rede (usb_do_poll a 1kHz no net_hook). O main lê
 *     o estado com get_controller_data(). g_pad_forward = 0 no menu: lê o
 *     controle mas não manda pro jogo.
 *   - console/fb: só o main (menu_ui.c desenha direto no fb; stdout vira
 *     log no rodapé). No jogo a GPU tira o console da tela no 1º flip.
 *   - Xe_*: só o main. Xe_Init uma vez no boot (antes de qualquer thread,
 *     como na v0.15); entre jogos gpu_present_finish/gpu_present_init.
 * ============================================================ */
#ifndef XCLOUD360_CTRL_PORT
#define XCLOUD360_CTRL_PORT 9002
#endif

#define EXIT_STREAM_END 0    /* stream embutido acabou / erro de decode */
#define EXIT_USER       1    /* Back+Start 2s */
#define EXIT_PC_IDLE    2    /* launcher mandou STATE IDLE no meio do jogo */
#define EXIT_NATIVE_END 3    /* stage 5: a sessão do Xbox acabou (ERROR/IDLE) */

static menu_model_t g_mm;
static int          g_stop_pending = 0;   /* STOP não saiu (desconectado) */
static uint64_t     g_hold_since = 0;     /* Back+Start apertados desde */

#ifdef XCLOUD360_NATIVE
static int      g_native_ok = 0;   /* xn_init ok + thread de rede de pé */
static uint64_t g_t_video_first = 0;   /* v0.19.2: 1o AU nativo no ring (espera a hw3) */
static unsigned g_xn_seen = 0;
/* Main: linha STATE da sessão do Xbox -> menu_model (mesmos códigos e
 * textos do launcher do PC). 1 se mudou. */
static int native_poll_state(void) {
    char l[240];
    unsigned s = xn_state_line(l, sizeof(l));
    if (!s || s == g_xn_seen) return 0;
    g_xn_seen = s;
    mm_handle_line(&g_mm, l);
    return 1;
}
#endif

/* Zera IDR/AU entre sessões: decoder novo precisa de IDR, conexão nova
 * começa num (capture-rtp), e os contadores do STAT são por jogo. */
static void video_session_reset(void) {
    g_pending_au_size = 0;
    g_wait_idr = 1;
    g_last_kfrq_us = 0;
    g_skipped_aus = 0;
    g_backlog_events = 0;
    g_decode_errors = 0;
}

static void send_stop(void) {
    if (ctrl_send_line("STOP") != 0) g_stop_pending = 1;   /* reenvia ao reconectar */
}

/* Fim do jogo pedido pelo Xbox: sessão nativa = DELETE pela thread da
 * sessão (stage 5); PC = STOP pro launcher. */
static void stop_game(void) {
#ifdef XCLOUD360_NATIVE
    if (g_native_game) { xn_stop(); return; }
#endif
    send_stop();
}

/* No jogo, a cada ~10ms: Back+Start por 2s = volta pro menu; a cada 100ms
 * drena a conexão de controle (o PC pode dizer que o jogo acabou). */
static int ingame_check(uint64_t now, int use_tcp) {
    static uint64_t last_ctrl = 0;
    struct controller_data_s d;
    if (!g_input_on_net) usb_do_poll();   /* sem thread de rede o USB é do main */
    get_any_pad(&d);
    if (d.back && d.start) {
        if (!g_hold_since) g_hold_since = now;
        else if (now - g_hold_since >= 2000000) { g_hold_since = 0; return EXIT_USER; }
    } else {
        g_hold_since = 0;
    }
    if (now - last_ctrl >= 100000) {
        last_ctrl = now;
        char line[512];
#ifdef XCLOUD360_NATIVE
        if (g_native_game) {
            /* o launcher do PC não manda neste jogo: só esvazia a fila */
            while (ctrl_recv_line(line, sizeof(line))) {}
            /* RECONNECTING/STARTING (re-sinalização) ficam no jogo: o vídeo
             * congela e volta sozinho. ERROR / IDLE = acabou. */
            if (native_poll_state() && (g_mm.state == MM_ST_ERROR || g_mm.state == MM_ST_IDLE))
                return EXIT_NATIVE_END;
            return 0;
        }
#endif
        while (ctrl_recv_line(line, sizeof(line))) {
            int ev = mm_handle_line(&g_mm, line);
            /* Queda de vídeo não sai (o launcher reconecta e o cliente de
             * vídeo tenta a cada 500ms); só o PC dizendo IDLE = sem jogo. */
            if (use_tcp && ev == MM_EV_STATE && g_mm.state == MM_ST_IDLE) return EXIT_PC_IDLE;
        }
    }
    return 0;
}

/* Volta do jogo pro menu. Ordem:
 *  1. para de mandar o controle pro jogo e solta tudo (estado neutro), pra
 *     um STOP perdido não deixar Back+Start apertados no xCloud;
 *  2. tela: GPU espera o último frame e devolve o scanout pro fb do console
 *     (gpu_present_finish); no caminho de CPU espera o worker esvaziar o
 *     ring de slots (senão ele pinta vídeo por cima do menu);
 *  3. decoder: avcodec_close junta as threads de frame (pthread_join espera
 *     xenon_is_thread_task_running cair — o startup do libxenon zera o slot
 *     quando a task retorna), liberando as hw threads 4,2,5,3 pro próximo;
 *  4. vídeo: derruba a conexão :9000 e espera a thread de rede atender
 *     (stream_reconnects_done), descarta o ring; zera IDR/contadores. */
static void video_teardown(AVCodecContext *ctx, AVCodecParserContext *parser,
                           AVFrame *frame, int use_tcp, int gpu_on) {
#ifdef XCLOUD360_AUDIO
    audio_session_end();   /* v0.18: fade + silêncio já (menu é mudo) */
#endif
#ifdef XCLOUD360_NATIVE
    if (g_native_game) {
        /* stage 5: mesmo cuidado do PC — controles soltos no xCloud antes de
         * fechar —, depois DELETE (thread da sessão) e a thread de rede
         * destrói o PeerConnection no próximo passo. */
        g_pad_forward = 0;
        __asm__ __volatile__("sync" ::: "memory");
        g_pad_neutral_req = 1;
        udelay(20000);   /* hook a 1 kHz zera; o xcw manda em <= 4 ms */
        xn_stop();
    } else
#endif
    if (use_tcp) {
        uint8_t neutral[24];
        memset(neutral, 0, sizeof(neutral));
        g_pad_forward = 0;
        __asm__ __volatile__("sync" ::: "memory");
        input_udp_send(neutral, sizeof(neutral));
        /* A thread de rede pode ter lido g_pad_forward=1 logo antes e posto o
         * estado real na mailbox depois do neutro: manda de novo. */
        udelay(5000);
        input_udp_send(neutral, sizeof(neutral));
    }
#ifdef XCLOUD360_GPU
    if (gpu_on) {
        gpu_present_finish();   /* console fb de volta na tela + console_open */
    } else
#endif
    {
        uint64_t t0 = now_us();
        while (now_us() - t0 < 500000) {
            int busy = 0;
            for (int i = 0; i < RING_SLOTS; i++) busy |= g_ring[i].ready;
            if (!busy) break;
            udelay(1000);
        }
    }
    (void)gpu_on;
    ui_log_install();   /* printf daqui em diante vai pro rodapé do menu */
    if (parser) av_parser_close(parser);
    if (ctx) {
        avcodec_close(ctx);
        av_free(ctx);
    }
#ifdef XCLOUD360_NATIVE
    /* v0.19.2: threads de frame juntadas -> a hw 3 pode ir pro DELETE */
    if (g_dec_hw3) { xn_dec_release_hw3(); g_dec_hw3 = 0; }
    xn_set_dec_threads(0);
#endif
    if (frame) av_free(frame);
#ifdef XCLOUD360_NATIVE
    if (g_native_game) {
        /* inject: não há conexão pra derrubar; o menu descarta o resto */
        stream_take_reset();
        stream_discard();
        g_native_game = 0;
    } else
#endif
    if (use_tcp) {
        unsigned want = stream_reconnects_done() + 1;
        stream_force_reconnect();
        uint64_t t0 = now_us();
        while ((int)(stream_reconnects_done() - want) < 0 && now_us() - t0 < 1000000) {
            stream_poll();   /* no-op com a thread de rede */
            udelay(1000);
        }
        stream_take_reset();
    }
    video_session_reset();
    printf("[menu] jogo encerrado, decoder fechado\n");
}
#endif /* XCLOUD360_MENU */

/* v0.16: o pipeline de vídeo da v0.15 (abre o decoder, loop de AUs, decode,
 * GPU/worker, STAT), agora numa função pra poder rodar várias vezes a partir
 * do menu. Com MENU=0 roda uma vez como antes. Retorna EXIT_* (MENU) ou 0;
 * -1 = não abriu o decoder. */
static int run_video(int use_tcp, int gpu_on, int net_thread_on) {
    int exit_reason = 0;
    (void)net_thread_on;
#ifdef XCLOUD360_MENU
    uint64_t t_last_chk = 0;
    video_session_reset();
    g_hold_since = 0;
#ifdef XCLOUD360_GPU
    /* 1ª vez: já armado no boot (no-op). Depois de um finish: re-arma sem
     * Xe_Init (gpu_present.c gp_rearm). */
    if (gpu_on && gpu_present_init(FB_W, FB_H) != 0) {
        printf("[gpu] re-arm falhou\n");
        if (use_tcp) stop_game();   /* não deixa o PC transmitindo pra ninguém */
#ifdef XCLOUD360_NATIVE
        g_native_game = 0;
#endif
        stream_force_reconnect();
        return -1;
    }
#endif
    g_pad_forward = use_tcp;   /* teste offline: controle não vai pra lugar nenhum */
#endif

    AVCodec* codec = avcodec_find_decoder(CODEC_ID_H264);
    if (!codec) {
        printf("[err] no H264 decoder\n");
#ifdef XCLOUD360_MENU
        if (use_tcp) stop_game();
        video_teardown(NULL, NULL, NULL, use_tcp, gpu_on);
#endif
        return -1;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
#if XCLOUD360_DECODE_THREADS > 1
    /* v0.13: frame threading — N frames decodando em paralelo nas threads de
     * hardware 4/1/5 (deps/xenon-pthread). Custa N-1 frames de atraso fixo
     * (~16ms cada a 60fps), mas o decode deixa de ser o gargalo. LOW_DELAY
     * desliga frame threading no libavcodec 0.10, então fica de fora. */
    ctx->thread_count = XCLOUD360_DECODE_THREADS;
#if defined(XCLOUD360_NATIVE)
    {
        /* stage 5: conta as hw threads livres de verdade. v0.19.2: no jogo
         * nativo a sessão sai da hw 3 assim que o answer é publicado (as
         * fases ao vivo vão pra hw1), então o decoder volta a ter
         * NATIVE_DEC_THREADS = 4 (hw 4,2,5,3). A hw 3 é ALUGADA (xn_dec_claim_hw3):
         * se a thread da sessão ainda estiver nela (re-login, handshake...),
         * abre com 3 e o loop do jogo cresce pra 4 quando ela sair.
         * Login (hw 3) rodando no teste offline: idem, pela contagem. */
        int free_hw = 0;
        for (int id = 1; id < 6; id++)
            if (!xenon_is_thread_task_running(id)) free_hw++;
        int want = ctx->thread_count;
        if (g_native_game && want > XCLOUD360_NATIVE_DEC_THREADS) want = XCLOUD360_NATIVE_DEC_THREADS;
        g_dec_hw3 = 0;
        if (want >= XC_DEC_HW3_AT) {
            if (xn_dec_claim_hw3()) g_dec_hw3 = 1;
            else want = XC_DEC_HW3_AT - 1;
        }
        if (want > free_hw) want = free_hw;
        if (want < 1) want = 1;
        if (g_dec_hw3 && want < XC_DEC_HW3_AT) { xn_dec_release_hw3(); g_dec_hw3 = 0; }
        if (want != ctx->thread_count) {
            printf("[dec] threads %d -> %d (%s, %d hw threads livres)\n", ctx->thread_count, want,
                   g_native_game ? "hw " XC360_STR(XST_HW_THREAD) " com a sessao" : "livres", free_hw);
            ctx->thread_count = want;
        }
    }
#elif defined(XCLOUD360_GPU)
    {
        /* v0.15: cada thread do decoder precisa de uma thread de hardware
         * livre (deps/xenon-pthread não multiplexa; se faltar, avcodec_open2
         * falha). Livres = 6 - main - rede - worker de convert (só sem GPU). */
        int free_hw = 6 - 1 - (net_thread_on ? 1 : 0) - (gpu_on ? 0 : 1);
        if (ctx->thread_count > free_hw) {
            printf("[dec] threads %d -> %d (hw threads livres)\n", ctx->thread_count, free_hw);
            ctx->thread_count = free_hw;
        }
    }
#endif
    ctx->thread_type  = FF_THREAD_FRAME;
#else
    ctx->thread_count = 1;
    ctx->flags  |= CODEC_FLAG_LOW_DELAY;
#endif
    ctx->flags2 |= CODEC_FLAG2_FAST;
#if XCLOUD360_SKIP_DEBLOCK
    /* v0.14.2: sem deblocking (~20-30% do decode H.264). Em combate o xCloud
     * passa de 4-5Mbps ignorando o REMB e o decoder não fecha 60fps → backlog
     * → pula até IDR. Custo: bordas de bloco 8x8 mais visíveis em movimento.
     * NONREF não serve: todo frame do xCloud é referência. */
    ctx->skip_loop_filter = AVDISCARD_ALL;
#endif
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        printf("[err] avcodec_open2 failed\n");
#ifdef XCLOUD360_MENU
        if (use_tcp) stop_game();
        video_teardown(ctx, NULL, NULL, use_tcp, gpu_on);
#endif
        return -1;
    }
    printf("[dec] threads=%d active_type=%d (1=frame 2=slice) skip_deblock=%d\n",
           ctx->thread_count, ctx->active_thread_type, XCLOUD360_SKIP_DEBLOCK);
#ifdef XCLOUD360_NATIVE
    xn_set_dec_threads(ctx->thread_count);
    if (g_native_game) {
        char l[96];
        snprintf(l, sizeof(l), "[dec] jogo nativo: %d threads de frame%s", ctx->thread_count,
                 g_dec_hw3 ? " (hw 4,2,5,3)" : " (hw3 com a sessao)");
        np_log_line(l);
    }
#endif
#ifdef XCLOUD360_AUDIO
    /* v0.18: jogo = o que chega na :9004; teste offline = tom embutido. */
#ifdef XCLOUD360_NATIVE
    if (g_native_game) audio_session_begin(AUDIO_SRC_RTC);   /* stage 5: Opus do WebRTC */
    else
#endif
    audio_session_begin(use_tcp ? AUDIO_SRC_NET : AUDIO_SRC_TEST);
#endif
    AVCodecParserContext* parser = av_parser_init(codec->id);
    AVFrame* frame = avcodec_alloc_frame();
    AVPacket pkt;
    av_init_packet(&pkt);

    printf("[play] mode=%s -> decode -> BT.601 -> direct-FB tiled\n\n",
           use_tcp ? "TCP-live" : "embedded");
#ifdef XCLOUD360_GPU
    if (gpu_on) {
        printf("[play] display = Xenos pixel shader (console goes dark at first frame;\n"
               "       telemetry continues over UDP STAT)\n");
#ifdef XCLOUD360_TRACE_LATENCY
        gpu_present_set_flip_cb(trace_on_gpu_flip);
#endif
    }
#endif

    uint64_t t_wall_start = now_us();
    /* v0.14.2: 2s (era 10s pra dar tempo de filmar a tela; agora a telemetria
     * vai pro devbox via UDP "STAT"). */
#define REPORT_PERIOD_US 2000000ULL
    uint64_t t_next_report = t_wall_start + REPORT_PERIOD_US;
    uint64_t acc_dec = 0, acc_cvt = 0;
    int frames_since_report = 0;
    int total_frames = 0;
    /* v0.16: era static dentro do report — com várias sessões o total_frames
     * (local) recomeça em 0 e o diff ficaria negativo. */
    int prev_total_frames = 0;

    const uint8_t *data = embedded_stream;
    size_t left = embedded_stream_len;

    for (;;) {
        uint8_t *out_data = NULL;
        int out_size = 0;
#ifdef XCLOUD360_TRACE_LATENCY
        uint64_t trace_au_seq = 0;   /* válido só se use_tcp — mata warn de escopo */
#endif
#ifdef XCLOUD360_MENU
        {
            uint64_t tn = now_us();
            if (tn - t_last_chk >= 10000) {
                t_last_chk = tn;
                int r = ingame_check(tn, use_tcp);
                if (r) { exit_reason = r; break; }
#ifdef XCLOUD360_NATIVE
                /* v0.19.2: a sessão precisa da hw 3 no meio do jogo (re-login,
                 * handshake TLS completo, re-sinalização): empresta (decoder
                 * com 3) e pega de volta (4) quando ela voltar pra hw1. */
                if (g_native_game && use_tcp) {
                    int w = xn_dec_threads_wanted(ctx->thread_count, XCLOUD360_NATIVE_DEC_THREADS);
                    if (w != ctx->thread_count) {
                        AVCodecContext *nc = dec_reopen(codec, ctx, w);
                        ctx = nc;
                        if (!ctx) { exit_reason = EXIT_STREAM_END; break; }
                    }
                }
#endif
            }
        }
#endif
#ifdef XCLOUD360_AUDIO
        if (!g_input_on_net) audio_service();   /* v0.18: sem thread de rede */
#endif
#ifdef XCLOUD360_GPU
        /* v0.15: não bloqueia — troca o scanout pro frame mais novo que a GPU
         * já terminou. Roda a cada AU e a cada 1ms no caminho ocioso. */
        if (gpu_on) gpu_present_poll();
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
                if (!g_input_on_net) {
                    usb_do_poll();
                    poll_and_send_gamepad();   /* P11: input pump idle path */
                }
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
            if (!g_input_on_net) {
                usb_do_poll();             /* P11: drain USB reports even */
                poll_and_send_gamepad();   /* on active path or wireless */
            }

            /* v0.12.0: IDR gating — ver comentário em g_wait_idr. */
            int is_idr = au_has_idr(out_data, out_size);
            if (is_idr) {
                g_wait_idr = 0;
            } else {
                if (!g_wait_idr && stream_available() > BACKLOG_BYTES) {
                    g_wait_idr = 1;
                    g_backlog_events++;
                }
                if (g_wait_idr) {
                    g_skipped_aus++;
                    request_idr();
                    continue;
                }
            }
        } else {
#ifdef XCLOUD360_AUDIO
            /* v0.18: o vídeo embutido (~5 s, decodado sem relógio) recomeça
             * até o tom ter tocado 2 voltas. Começa num IDR. */
            if (left == 0 && now_us() - t_wall_start < AUDIO_TEST_US) {
                data = embedded_stream;
                left = embedded_stream_len;
            }
#endif
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
             * instead of tearing down the whole session. v0.12.0: de fato
             * espera o IDR (g_wait_idr) em vez de seguir decodando P-frames
             * contra a referência quebrada. */
            if (use_tcp) {
                g_decode_errors++;
                g_wait_idr = 1;
                request_idr();
                continue;
            }
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

#if XCLOUD360_DISPLAY_DIVISOR > 1
        /* v0.14.1: xCloud manda 60fps e todo frame é referência (nonref=0 em
         * 58k AUs), então o decode não pode pular nenhum. Mas a exibição pode:
         * mostra 1 a cada N → cadência fixa de 60/N fps em vez de 60 com
         * buracos irregulares, e o main deixa de copiar 1.4MB por frame
         * descartado (mesmo loop que alimenta o decoder em frame threading). */
        if (decoded_count % XCLOUD360_DISPLAY_DIVISOR) continue;
#endif
        uint64_t t_d;
#ifdef XCLOUD360_GPU
        if (gpu_on) {
            /* v0.15: o main sobe os planos pras texturas (set livre), desenha
             * e chuta a GPU sem esperar. prod_ms = chamada inteira; cvt_ms
             * no STAT = só a cópia pras texturas (upload). */
            int gW = frame->width  < FB_W ? frame->width  : FB_W;
            int gH = frame->height < FB_H ? frame->height : FB_H;
            uint64_t t_c = now_us();
            uint64_t tag = 0;
#ifdef XCLOUD360_TRACE_LATENCY
            if (use_tcp) {
                tag = trace_au_seq;
                g_trace[(int)(trace_au_seq % TRACE_HISTORY)].t_dispatch = t_c;
            }
#endif
            gpu_present_frame(frame->data, frame->linesize, gW, gH, tag);
            t_d = now_us();
            acc_cvt += (t_d - t_c);
            frames_since_report++;
            goto report;
        }
#endif
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
        t_d = now_us();

        /* v0.9.6: acc_dec / total_frames já contabilizados antes do skip. */
        acc_cvt += (t_d - t_c);       /* now = memcpy + backpressure wait */
        frames_since_report++;

#ifdef XCLOUD360_GPU
report:
#endif
        if (t_d >= t_next_report) {
            double sec = (t_d - (t_next_report - REPORT_PERIOD_US)) / 1e6;
            int dec_frames = total_frames - prev_total_frames;
            prev_total_frames = total_frames;
            /* acc_dec soma o tempo de TODOS os decodes do período; com frame
             * threading é o tempo que o main ficou preso no avcodec. */
            double dec_ms = dec_frames > 0 ? (double)acc_dec / 1000.0 / dec_frames : 0;
            double prod_ms = (double)acc_cvt / 1000.0 / frames_since_report;
            double fps    = frames_since_report / sec;
            double dec_fps = dec_frames / sec;
            int worker_fps_snapshot = g_worker_frames;
            /* v0.14.3: média do período (antes era desde o boot e escondia
             * mudanças). Leitura sem trava: o worker só soma, erro de 1 frame. */
            static uint64_t prev_cvt_total = 0;
            static int prev_worker_frames = 0;
            uint64_t cvt_total = g_cvt_us_total;
            int wf = worker_fps_snapshot - prev_worker_frames;
            double wcvt_ms = wf > 0 ? (double)(cvt_total - prev_cvt_total) / 1000.0 / wf : 0;
            prev_cvt_total = cvt_total;
            prev_worker_frames = worker_fps_snapshot;
            static uint64_t prev_bot_total = 0;
            uint64_t bot_total = g_bot_us_total;
            double bot_ms = wf > 0 ? (double)(bot_total - prev_bot_total) / 1000.0 / wf : 0;
            prev_bot_total = bot_total;
#ifdef XCLOUD360_GPU
            /* v0.15: com GPU não há worker nem metade de baixo. cvt_ms vira o
             * upload (cópia dos planos pras texturas + dcbst), bot_ms = 0, e
             * wF conta os flips de scanout. Extras no fim do STAT. */
            double gwait_ms = 0, flip_lag_ms = 0;
            unsigned gflips = 0, gsuper = 0, gtimeouts = 0;
            if (gpu_on) {
                static gpu_present_stats_t prev_gs;
                gpu_present_stats_t gs;
                gpu_present_get_stats(&gs);
                uint64_t gf = gs.frames - prev_gs.frames;
                uint64_t fl = gs.flips - prev_gs.flips;
                wcvt_ms  = gf ? (double)(gs.upload_us - prev_gs.upload_us) / 1000.0 / gf : 0;
                gwait_ms = gf ? (double)(gs.wait_us - prev_gs.wait_us) / 1000.0 / gf : 0;
                flip_lag_ms = fl ? (double)(gs.flip_lag_us - prev_gs.flip_lag_us) / 1000.0 / fl : 0;
                bot_ms = 0;
                worker_fps_snapshot = (int)gs.flips;
                gflips = (unsigned)fl;
                gsuper = gs.superseded - prev_gs.superseded;
                gtimeouts = gs.timeouts;
                prev_gs = gs;
            }
#endif
            size_t ring_bytes = stream_available();
            printf("[play] t+%3ds dec=%.1f prod=%.1f w_cvt=%.1f fps=%.1f wF=%d ring=%zu\n",
                (int)((t_d - t_wall_start)/1000000),
                dec_ms, prod_ms, wcvt_ms, fps, worker_fps_snapshot, ring_bytes);
            printf("[idr]  skipped=%u backlog=%u dec_err=%u\n",
                g_skipped_aus, g_backlog_events, g_decode_errors);
            /* v0.14.2: telemetria pro capture-rtp logar (não dá pra fotografar
             * a tela a tempo). Sai pela thread de rede junto com o controle. */
            if (use_tcp) {
#if defined(XCLOUD360_NATIVE)
                char stat[580];   /* stage 5: + chaves do WebRTC; sai pelo LOG (:9003) */
#elif defined(XCLOUD360_AUDIO)
                char stat[320];   /* = STAT_MAX do input_udp.c (v0.18: + áudio) */
#elif defined(XCLOUD360_GPU)
                char stat[240];   /* = STAT_MAX do input_udp.c */
#else
                char stat[200];
#endif
                int n = snprintf(stat, sizeof(stat),
                    "STAT t=%d dec_fps=%.1f disp_fps=%.1f dec_ms=%.1f prod_ms=%.1f "
                    "cvt_ms=%.1f bot_ms=%.1f ring=%u skipped=%u backlog=%u dec_err=%u",
                    (int)((t_d - t_wall_start)/1000000), dec_fps, fps, dec_ms,
                    prod_ms, wcvt_ms, bot_ms, (unsigned)ring_bytes,
                    g_skipped_aus, g_backlog_events, g_decode_errors);
#ifdef XCLOUD360_GPU
                /* Chaves novas só no FIM: as de cima ficam iguais. */
                if (gpu_on && n > 0 && n < (int)sizeof(stat)) {
                    int m = snprintf(stat + n, sizeof(stat) - n,
                        " gpu=1 gwait_ms=%.2f flip_ms=%.1f flips=%u sup=%u gto=%u",
                        gwait_ms, flip_lag_ms, gflips, gsuper, gtimeouts);
                    if (m > 0) n += m;
                    if (n >= (int)sizeof(stat)) n = (int)sizeof(stat) - 1;
                }
#endif
                if (g_input_on_net && n > 0 && n < (int)sizeof(stat)) {
                    uint32_t ug = g_ugap_max_us;
                    g_ugap_max_us = 0;
                    int m = snprintf(stat + n, sizeof(stat) - n,
                        " ugap_ms=%.1f ugaps=%u", ug / 1000.0, (unsigned)g_ugaps);
                    if (m > 0) n += m;
                    if (n >= (int)sizeof(stat)) n = (int)sizeof(stat) - 1;
                }
#ifdef XCLOUD360_AUDIO
                /* v0.18: chaves do áudio no FIM (as de cima ficam iguais). */
                if (n > 0 && n < (int)sizeof(stat)) n += audio_stat(stat + n, sizeof(stat) - n);
#endif
#ifdef XCLOUD360_NATIVE
                /* stage 5: sem capture-rtp pra logar o STAT da :9001 -> o
                 * listener de LOG do PC (UDP :9003) recebe "LOG STAT ...",
                 * com as chaves do WebRTC no fim (rtt, perdas, kbps, NACK,
                 * keyframes, SRTP, carga da hw1). */
                if (g_native_game) {
                    if (n > 0 && n < (int)sizeof(stat)) n += xn_stat_tail(stat + n, sizeof(stat) - n);
                    if (n > 0) np_log_line(stat);
                } else
#endif
                if (n > 0) input_udp_send_stat(stat, (size_t)n);
            }
            acc_dec = acc_cvt = 0; frames_since_report = 0;
            t_next_report = t_d + REPORT_PERIOD_US;
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
#ifdef XCLOUD360_MENU
    /* v0.16: STOP sai antes do teardown, pro PC já ir fechando o jogo. */
#ifdef XCLOUD360_NATIVE
    /* nativo: o video_teardown solta os controles no xCloud e só então faz
     * o xn_stop (DELETE + PeerConnection destruída) */
    if (exit_reason == EXIT_USER && use_tcp && !g_native_game) send_stop();
#else
    if (exit_reason == EXIT_USER && use_tcp) send_stop();
#endif
    uint64_t t_wall_end = now_us();
    video_teardown(ctx, parser, frame, use_tcp, gpu_on);
#else
    /* Signal worker to drain and exit */
    g_producer_done = 1;
#ifdef XCLOUD360_AUDIO
    audio_session_end();
#endif
#ifdef XCLOUD360_GPU
    /* v0.15: com GPU a hw thread 2 é do decoder (nunca termina) — não esperar
     * por ela. Espera a GPU, devolve o scanout pro console e reabre o printf. */
    if (gpu_on) gpu_present_finish();
    else
#endif
    while (g_worker_frames < total_frames && xenon_is_thread_task_running(2)) {
        for (volatile int i = 0; i < 10000; i++) {}
    }

    uint64_t t_wall_end = now_us();
#endif
    double wall_s = (t_wall_end - t_wall_start) / 1e6;
    double fps = total_frames / wall_s;
    printf("\n==== RESULT ====\n");
    printf("frames         : %d\n", total_frames);
    printf("wall time      : %.2f s\n", wall_s);
    printf("effective fps  : %.2f\n", fps);
    if (fps >= 55.0)      printf("verdict        : PASS\n");
    else if (fps >= 30.0) printf("verdict        : PARTIAL\n");
    else                  printf("verdict        : FAIL\n");
    return exit_reason;
}

#ifdef XCLOUD360_MENU
/* ---- tela do menu (menu_ui.c desenha, menu_model.c guarda o estado) ---- */

#define MENU_GO_STREAM 1
#define MENU_GO_TEST   2

enum { SCR_LIST = 0, SCR_LOGIN, SCR_WAIT, SCR_NONET };

/* Layout em px pra 720p; o rodapé fica dentro da área segura de TV (~25px). */
#define UI_X        48
#define UI_TITLE_Y  28
#define UI_STAT_Y   76
#define UI_DET_Y    112
#define UI_LIST_Y   152
#define UI_ROW_H    32
#define UI_ROWS     13          /* 152 + 13*32 = 568 */
#define UI_HELP_Y   584
#define UI_LOG_Y    608         /* 4 linhas de 16px -> 672 */

#define C_BG     UI_RGB(14, 18, 28)
#define C_BAR    UI_RGB(16, 110, 16)     /* verde Xbox */
#define C_SEL    UI_RGB(40, 90, 150)
#define C_WHITE  UI_RGB(240, 240, 240)
#define C_DIM    UI_RGB(150, 155, 170)
#define C_LOG    UI_RGB(100, 105, 120)
#define C_YELLOW UI_RGB(255, 210, 60)
#define C_RED    UI_RGB(255, 110, 90)
#define C_GREEN  UI_RGB(120, 230, 120)

static int  g_scr_drawn = -1;
static char g_login_drawn[64];

#ifdef XCLOUD360_LOGIN
/* v0.17: de onde vem o catálogo. 0 = login do Xbox em andamento,
 * 1 = catálogo do Xbox na tela, -1 = Xbox falhou -> launcher (v0.16). */
static int       g_cat_src = 0;
static int       g_xbox_login = 0;       /* tela de código do Xbox */
static unsigned  g_lt_runs_seen = 0;
static lt_snap_t g_lts;
static char      g_xbox_err[170] = "";

#ifdef XCLOUD360_NATIVE
/* stage 5: flags do catálogo (XA_F_ADPLAY = jogo com anúncio -> oferta
 * xgpuwebf2p) por titleId; o menu_model só guarda id/nome. */
static struct { char id[MM_ID_LEN]; unsigned flags; } g_xflags[MM_MAX_GAMES];
static int g_nxflags = 0;
static unsigned xbox_title_flags(const char *id) {
    for (int i = 0; i < g_nxflags; i++)
        if (!strcmp(g_xflags[i].id, id)) return g_xflags[i].flags;
    return 0;
}
#endif

/* Lista do Xbox -> menu_model, pelas mesmas linhas GAME/END do launcher. */
static void xbox_catalog_commit(void) {
    xa_title_t *t = NULL;
    int n = 0;
    if (lt_take_games(&t, &n) != 0) return;
    char line[16 + MM_ID_LEN + MM_NAME_LEN];
#ifdef XCLOUD360_NATIVE
    g_nxflags = 0;
#endif
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "GAME\t%s\t%s\t0", t[i].id, t[i].name[0] ? t[i].name : t[i].id);
        mm_handle_line(&g_mm, line);
#ifdef XCLOUD360_NATIVE
        if (g_nxflags < MM_MAX_GAMES) {
            snprintf(g_xflags[g_nxflags].id, MM_ID_LEN, "%.*s", MM_ID_LEN - 1, t[i].id);
            g_xflags[g_nxflags++].flags = t[i].flags;
        }
#endif
    }
    snprintf(line, sizeof(line), "END");
    mm_handle_line(&g_mm, line);
    free(t);
}

/* Uma vez por volta do menu. Retorna 1 se o launcher deve mandar a lista. */
static int xbox_login_poll(void) {
    lt_snapshot(&g_lts);
    g_xbox_login = g_lts.phase == LT_RUNNING && g_lts.uri[0];
    if (g_xbox_login) {
        snprintf(g_mm.login_url, sizeof(g_mm.login_url), "%s", g_lts.uri);
        snprintf(g_mm.login_code, sizeof(g_mm.login_code), "%s", g_lts.code);
    }
    if (g_lts.runs == g_lt_runs_seen) return 0;
    g_lt_runs_seen = g_lts.runs;
    if (g_lts.phase == LT_DONE) {
        xbox_catalog_commit();
        g_cat_src = 1;
        g_xbox_err[0] = 0;
        printf("[menu] catalogo: Xbox (%d jogos)\n", g_mm.ngames);
        return 0;
    }
    if (g_lts.phase == LT_FAILED) {
        snprintf(g_xbox_err, sizeof(g_xbox_err), "Xbox: %s", g_lts.err);
        /* Já tinha catálogo do Xbox (Y falhou): fica com ele. */
        if (g_cat_src != 1) { g_cat_src = -1; return 1; }
    }
    return 0;
}
#endif

typedef struct { int up, down, left, right, a, b, x, y, lb, rb, rt; } menu_btn_t;

/* Controle no menu: D-pad ou stick esquerdo. Sem usb_do_poll aqui quando a
 * thread de rede está de pé (ela é a dona do USB). */
static void menu_read_buttons(menu_btn_t *b) {
    struct controller_data_s d;
    if (!g_input_on_net) usb_do_poll();
    else if (now_us() - g_usb_poll_us > 40000) {
        /* v0.17.1: USB parado (pendrive em uso / rede ocupada): estado velho.
         * Direções contam como soltas (zera a repetição); botões ficam como
         * estavam, pra não virar um "aperto novo" quando o USB voltar. */
        b->up = b->down = b->left = b->right = 0;
        return;
    }
    get_any_pad(&d);
    b->up    = d.up    || d.s1_y >  20000;   /* libxenon: Y positivo = cima */
    b->down  = d.down  || d.s1_y < -20000;
    b->left  = d.left  || d.s1_x < -20000;
    b->right = d.right || d.s1_x >  20000;
    b->a = d.a; b->b = d.b; b->x = d.x; b->y = d.y; b->lb = d.lb; b->rb = d.rb;
    b->rt = d.rt > 128;   /* stage 5: RT segurado + A = jogar via PC */
}

/* Repetição de tecla segurada: dispara ao apertar, de novo após 400ms e
 * depois a cada 80ms. */
static int key_repeat(int held, uint64_t now, uint64_t *next) {
    if (!held) { *next = 0; return 0; }
    if (*next == 0) { *next = now + 400000; return 1; }
    if (now >= *next) { *next = now + 80000; return 1; }
    return 0;
}

static void menu_status(char *st, char *det, size_t n, uint32_t *col,
                        int screen, int conn, const char *play_name, const char *notice) {
    det[0] = '\0';
    *col = C_WHITE;
    if (screen == SCR_NONET) {
        snprintf(st, n, "Sem rede: network_init falhou");
        snprintf(det, n, "Verifique o cabo e reinicie o Xbox. X: teste offline");
        *col = C_RED;
        return;
    }
#ifdef XCLOUD360_LOGIN
    if (screen == SCR_LOGIN && g_xbox_login) {
        snprintf(st, n, "Faca login: acesse %s e digite %s", g_mm.login_url, g_mm.login_code);
        snprintf(det, n, "%s", g_lts.status);
        *col = C_YELLOW;
        return;
    }
    if (screen == SCR_LIST && g_cat_src == 0) {
        snprintf(st, n, "%s", g_lts.status[0] ? g_lts.status : "Login do Xbox...");
        snprintf(det, n, "catalogo: Xbox (carregando)%s", conn ? "" : "  |  PC desconectado");
        *col = C_YELLOW;
        return;
    }
#ifdef XCLOUD360_NATIVE
    /* stage 5: jogo sem PC. A espera mostra os STATE da sessão do Xbox
     * (mesmos códigos do launcher) sem depender da conexão com o PC. */
    if (screen == SCR_WAIT && g_native_game) conn = 1;
    if (screen == SCR_LIST && g_cat_src == 1 && g_native_ok && !conn) {
        int calm = g_mm.state == MM_ST_NONE || g_mm.state == MM_ST_IDLE || g_mm.state == MM_ST_UNKNOWN ||
                   g_mm.state == MM_ST_CATALOG;
        snprintf(st, n, "Escolha um jogo  -  catalogo: Xbox (%d jogos), sem PC", g_mm.ngames);
        if (notice && notice[0]) snprintf(det, n, "%s", notice);
        else if (!calm && g_mm.state_text[0]) snprintf(det, n, "%s", g_mm.state_text);
        else snprintf(det, n, "A: jogar   (PC desconectado: RT+A indisponivel)");
        *col = g_mm.state == MM_ST_ERROR && !(notice && notice[0]) ? C_RED : C_WHITE;
        return;
    }
#endif
    if (screen == SCR_LIST && g_cat_src == 1 && !conn) {
        snprintf(st, n, "catalogo: Xbox (%d jogos)  -  PC desconectado", g_mm.ngames);
        snprintf(det, n, "Para jogar, abra o launcher no PC (%s:%d)", XCLOUD360_STREAM_SERVER_IP,
                 XCLOUD360_CTRL_PORT);
        *col = C_YELLOW;
        return;
    }
#endif
    if (!conn) {
        snprintf(st, n, screen == SCR_WAIT ? "Reconectando ao PC..." : "Conectando ao PC...");
        snprintf(det, n, "Abra o launcher no PC (%s:%d)", XCLOUD360_STREAM_SERVER_IP,
                 XCLOUD360_CTRL_PORT);
        *col = C_YELLOW;
        return;
    }
    switch (g_mm.state) {
    case MM_ST_LOGIN:
        snprintf(st, n, "Faca login: acesse %s e digite %s", g_mm.login_url, g_mm.login_code);
        *col = C_YELLOW;
        break;
    case MM_ST_CATALOG:      snprintf(st, n, "Carregando catalogo..."); break;
    case MM_ST_STARTING:
        snprintf(st, n, "Iniciando %s...", play_name[0] ? play_name : "jogo");
        *col = C_GREEN;
        break;
    case MM_ST_QUEUE:        snprintf(st, n, "Na fila"); *col = C_YELLOW; break;
    case MM_ST_STREAMING:
        snprintf(st, n, screen == SCR_WAIT ? "Abrindo o video..." : "Jogo em andamento no PC");
        *col = C_GREEN;
        break;
    case MM_ST_RECONNECTING: snprintf(st, n, "Reconectando..."); *col = C_YELLOW; break;
    case MM_ST_ERROR:        snprintf(st, n, "Erro"); *col = C_RED; break;
    default:
        if (screen == SCR_WAIT) {
            snprintf(st, n, "Iniciando %s...", play_name);
            *col = C_GREEN;
        } else {
            snprintf(st, n, "Escolha um jogo");
        }
        break;
    }
    if (g_mm.state_text[0] && g_mm.state != MM_ST_LOGIN) snprintf(det, n, "%s", g_mm.state_text);
    if (!det[0] && notice && notice[0]) snprintf(det, n, "%s", notice);
#ifdef XCLOUD360_LOGIN
    if (screen == SCR_LIST) {
        /* PC parado (sem jogo pedido): a linha de status diz de onde veio a lista */
        int calm = g_mm.state == MM_ST_NONE || g_mm.state == MM_ST_IDLE ||
                   g_mm.state == MM_ST_UNKNOWN || g_mm.state == MM_ST_CATALOG;
        if (g_cat_src == 1 && calm) {
            snprintf(st, n, "Escolha um jogo  -  catalogo: Xbox (%d jogos)", g_mm.ngames);
            *col = C_WHITE;
        } else if (g_cat_src < 0 && g_xbox_err[0] && (!det[0] || (calm && !(notice && notice[0])))) {
            snprintf(det, n, "%s - usando o catalogo do PC", g_xbox_err);
        }
    }
#endif
}

static void menu_draw(int screen, int conn, const char *play_name, const char *notice) {
    char st[256], det[256], t[200];   /* 256: url(160) + codigo(32) + texto */
    uint32_t stcol;
    int W = ui_width();
    int cols2 = (W - 2 * UI_X) / 16;
    int cols1 = (W - 2 * UI_X) / 8;
    if (cols2 > 150) cols2 = 150;
    if (cols1 > 150) cols1 = 150;

    /* Código de login mudou de tamanho: o desenho grande (centralizado) não
     * cobriria o anterior — redesenha a tela toda. */
    if (screen == SCR_LOGIN && strcmp(g_login_drawn, g_mm.login_code) != 0) g_scr_drawn = -1;
    if (screen != g_scr_drawn) {
        /* Troca de tela: limpa uma vez (~15ms). Dentro da mesma tela só as
         * linhas que mudaram são reescritas (cache do ui_line), sem piscar. */
        ui_clear(C_BG);
        ui_fill(0, UI_TITLE_Y - 10, W, 52, C_BAR);
        g_scr_drawn = screen;
        snprintf(g_login_drawn, sizeof(g_login_drawn), "%s", g_mm.login_code);
    }

    char right[32] = "";
    if (screen == SCR_LIST && g_mm.ngames > 0)
        snprintf(right, sizeof(right), "%d/%d", g_mm.cursor + 1, g_mm.ngames);
    snprintf(t, sizeof(t), "XCloud360  v%s", XCLOUD360_BUILD_VERSION);
    int pad = cols2 - (int)strlen(t) - (int)strlen(right);
    if (pad > 0) {
        size_t l = strlen(t);
        for (int i = 0; i < pad && l + 1 < sizeof(t); i++) t[l++] = ' ';
        t[l] = '\0';
        strncat(t, right, sizeof(t) - strlen(t) - 1);
    }
    ui_line(0, UI_X, UI_TITLE_Y, 2, C_WHITE, C_BAR, cols2, t);

    menu_status(st, det, sizeof(st), &stcol, screen, conn, play_name, notice);
    ui_line(1, UI_X, UI_STAT_Y, 2, stcol, C_BG, cols2, st);
    ui_line(2, UI_X, UI_DET_Y, 2, C_DIM, C_BG, cols2, det);

    const char *help = "";
    if (screen == SCR_LIST) {
        for (int r = 0; r < UI_ROWS; r++) {
            int idx = g_mm.top + r;
            int y = UI_LIST_Y + r * UI_ROW_H;
            if (idx < g_mm.ngames) {
                const mm_game_t *g = &g_mm.games[idx];
                int sel = (idx == g_mm.cursor);
                snprintf(t, sizeof(t), "%s%s%s", sel ? "> " : "  ", g->name,
                         g->recent ? "  (recente)" : "");
                ui_line(3 + r, UI_X, y, 2, sel ? C_WHITE : (g->recent ? C_YELLOW : C_WHITE),
                        sel ? C_SEL : C_BG, cols2, t);
            } else if (r == 0 && g_mm.ngames == 0) {
                const char *empty = g_mm.catalogs ? "Nenhum jogo no catalogo. Y: atualizar"
                                                  : (conn ? "Aguardando o catalogo do PC..." : "");
#ifdef XCLOUD360_LOGIN
                if (g_cat_src == 0 && !g_mm.catalogs) empty = "Carregando o catalogo pelo Xbox...";
#endif
                ui_line(3 + r, UI_X, y, 2, C_DIM, C_BG, cols2, empty);
            } else {
                ui_line(3 + r, UI_X, y, 2, C_WHITE, C_BG, cols2, "");
            }
        }
        help = "Cima/Baixo: mover   Esq/Dir: pagina   LB/RB: letra   A: jogar   Y: atualizar   "
               "X: teste offline   |   No jogo: Back+Start 2s = menu";
#ifdef XCLOUD360_NATIVE
        if (g_native_ok && g_cat_src == 1)
            help = "Cima/Baixo: mover  Esq/Dir: pagina  LB/RB: letra  A: jogar  RT+A: via PC  Y: atualizar  "
                   "X: teste  |  No jogo: Back+Start 2s";
#endif
    } else if (screen == SCR_LOGIN) {
        const char *code = g_mm.login_code;
        int len = (int)strlen(code);
        int s = 6;
        while (s > 2 && len * 8 * s > W - 2 * UI_X) s--;
        int code_y = UI_LIST_Y + 6 * UI_ROW_H;
        ui_line(3, UI_X, UI_LIST_Y, 2, C_WHITE, C_BG, cols2, "Faca login na sua conta Microsoft:");
        ui_line(4, UI_X, UI_LIST_Y + 2 * UI_ROW_H, 2, C_DIM, C_BG, cols2,
                "1. No celular ou no PC, acesse:");
        ui_line(5, UI_X + 32, UI_LIST_Y + 3 * UI_ROW_H, 2, C_YELLOW, C_BG, cols2 - 2, g_mm.login_url);
        ui_line(6, UI_X, UI_LIST_Y + 5 * UI_ROW_H, 2, C_DIM, C_BG, cols2, "2. Digite o codigo:");
        if (len > 0) ui_line(7, (W - len * 8 * s) / 2, code_y, s, C_YELLOW, C_BG, len, code);
        const char *foot = "O menu continua sozinho quando o login terminar.";
#ifdef XCLOUD360_LOGIN
        if (g_xbox_login) foot = "O login fica salvo no pendrive (XC360/TOKEN.TXT).";
#endif
        ui_line(8, UI_X, code_y + 16 * s + 16, 2, C_DIM, C_BG, cols2, foot);
        help = "Y: atualizar   X: teste offline   B: cancelar o jogo pedido";
#ifdef XCLOUD360_LOGIN
        if (g_xbox_login) help = "B: pular o login (usar o catalogo do PC)";
#endif
    } else if (screen == SCR_WAIT) {
        snprintf(t, sizeof(t), "Iniciando %s...", play_name);
        ui_line(3, UI_X, UI_LIST_Y + UI_ROW_H, 2, C_WHITE, C_BG, cols2, t);
        const char *wtxt = "O video aparece assim que o jogo abrir no PC.";
#ifdef XCLOUD360_NATIVE
        if (g_native_game) wtxt = "Sessao no proprio Xbox (sem PC). O video aparece sozinho.";
#endif
        ui_line(4, UI_X, UI_LIST_Y + 3 * UI_ROW_H, 2, C_DIM, C_BG, cols2, wtxt);
        ui_line(5, UI_X, UI_LIST_Y + 5 * UI_ROW_H, 2, C_DIM, C_BG, cols2, "B: cancelar");
        ui_line(6, UI_X, UI_LIST_Y + 6 * UI_ROW_H, 2, C_DIM, C_BG, cols2,
                "No jogo: segure Back + Start por 2 s para voltar ao menu.");
        help = "B: cancelar";
    } else {
        ui_line(3, UI_X, UI_LIST_Y + UI_ROW_H, 2, C_WHITE, C_BG, cols2,
                "Sem rede o menu nao funciona.");
        ui_line(4, UI_X, UI_LIST_Y + 3 * UI_ROW_H, 2, C_DIM, C_BG, cols2,
                "X: tocar o stream de teste embutido");
        help = "X: teste offline";
    }
    ui_line(16, UI_X, UI_HELP_Y, 1, C_DIM, C_BG, cols1, help);

    char lg[UI_LOG_LINES][UI_LOG_COLS];
    ui_log_get(lg);
    for (int i = 0; i < UI_LOG_LINES; i++)
        ui_line(17 + i, UI_X, UI_LOG_Y + i * 16, 1, C_LOG, C_BG, cols1, lg[i]);
    ui_flush();
}

/* Loop do menu (main thread, ~12ms por volta). Retorna quando é hora de
 * entrar no pipeline de vídeo: MENU_GO_STREAM (chegou vídeo depois de um
 * PLAY) ou MENU_GO_TEST (X = stream embutido). */
static int menu_loop(int net_ok, const char *msg) {
    ui_log_install();
    ui_attach();
    g_scr_drawn = -1;

    int waiting = 0;
    unsigned want_reconn = 0;
    char play_name[MM_NAME_LEN] = "";
    char notice[120];
    snprintf(notice, sizeof(notice), "%s", msg ? msg : "");
    unsigned seen_gen = ctrl_conn_gen();
    int need_list = 1;   /* recentes podem ter mudado com o jogo que acabou */
    uint64_t rep_up = 0, rep_dn = 0;
    menu_btn_t prev, b;
    /* Botões já apertados na entrada (Back+Start do jogo, A do teste) não
     * contam como clique. */
    menu_read_buttons(&prev);

    for (;;) {
        uint64_t now = now_us();
        if (net_ok) stream_poll();   /* no-op com a thread de rede */
#ifdef XCLOUD360_AUDIO
        if (!g_input_on_net) audio_service();   /* termina o fade do jogo */
#endif

        char line[512];
#ifdef XCLOUD360_LOGIN
        if (xbox_login_poll()) need_list = 1;
        while (ctrl_recv_line(line, sizeof(line))) {
            /* Catálogo do Xbox (ou o login dele em andamento) manda: GAME/END
             * do launcher só valem no fallback. */
            if (g_cat_src >= 0 && (!strncmp(line, "GAME\t", 5) || !strcmp(line, "END"))) continue;
#ifdef XCLOUD360_NATIVE
            /* jogo nativo: o STATE que vale é o da sessão do Xbox */
            if (g_native_game && !strncmp(line, "STATE\t", 6)) continue;
#endif
            mm_handle_line(&g_mm, line);
        }
#else
        while (ctrl_recv_line(line, sizeof(line))) mm_handle_line(&g_mm, line);
#endif
#ifdef XCLOUD360_NATIVE
        /* sessão encerrando depois do jogo / do B: o STATE dela (IDLE
         * "Sessao encerrada") segue chegando no menu */
        if (!waiting && !g_native_game && xn_busy()) native_poll_state();
        if (g_native_game && waiting && native_poll_state() &&
            (g_mm.state == MM_ST_ERROR || g_mm.state == MM_ST_IDLE)) {
            /* a sessão do Xbox desistiu (erro com texto em pt-BR) ou acabou:
             * volta pra lista com o motivo; xn_stop solta o WebRTC */
            printf("[menu] sessao do Xbox: %s\n", g_mm.state_text);
            xn_stop();
            g_native_game = 0;
            waiting = 0;
            play_name[0] = '\0';
            snprintf(notice, sizeof(notice), "%.*s", (int)sizeof(notice) - 1,
                     g_mm.state_text[0] ? g_mm.state_text : "Sessao encerrada");
        }
#endif
        int conn = ctrl_connected();
        if (conn) {
            unsigned gen = ctrl_conn_gen();
            if (gen != seen_gen) {
                seen_gen = gen;
                g_mm.receiving = 0;   /* GAMEs pela metade eram da conexão velha */
                need_list = 1;
            }
            if (g_stop_pending && ctrl_send_line("STOP") == 0) g_stop_pending = 0;
#ifdef XCLOUD360_LOGIN
            if (g_cat_src >= 0) need_list = 0;   /* catálogo é do Xbox */
#endif
            if (need_list && !g_stop_pending && ctrl_send_line("LIST") == 0) need_list = 0;
        }

        /* Vídeo: fora de um PLAY, o que chegar na :9000 é sobra (capture-rtp
         * anterior) — joga fora. Depois do PLAY, só vale o que vier de uma
         * conexão aberta depois do nosso stream_force_reconnect. */
        if (net_ok) {
            if (!waiting) {
                stream_take_reset();
                stream_discard();
#ifdef XCLOUD360_NATIVE
            } else if (g_native_game) {
                /* stage 5: o WebRTC só injeta depois do 1o IDR do PC novo;
                 * o ring foi esvaziado no PLAY (sem produtor até ali). */
                stream_take_reset();
                if (stream_available() >= 4) {
                    /* v0.19.2: a thread da sessão entrega as fases ao vivo pra
                     * hw1 e sai da hw 3 logo depois do answer; espera ela
                     * (até 2 s) pro decoder abrir com as 4 threads. */
                    if (!g_t_video_first) g_t_video_first = now;
                    if (xn_session_thread_busy() && now - g_t_video_first < 2000000) {
                        /* segue no menu mais um pouco */
                    } else {
                        printf("[menu] video do Xbox chegou (%u B), entrando no jogo%s\n",
                               (unsigned)stream_available(),
                               xn_session_thread_busy() ? " (sessao ainda na hw3)" : "");
                        g_t_video_first = 0;
                        return MENU_GO_STREAM;
                    }
                }
#endif
            } else if ((int)(stream_reconnects_done() - want_reconn) >= 0) {
                stream_take_reset();
                if (stream_available() >= 4) {
                    printf("[menu] video chegou (%u B), entrando no jogo\n",
                           (unsigned)stream_available());
                    return MENU_GO_STREAM;
                }
            }
        }

        /* Login tem prioridade até sobre "Iniciando": o token pode vencer
         * justo no PLAY, e o código precisa aparecer grande. */
        int screen = !net_ok ? SCR_NONET
#ifdef XCLOUD360_LOGIN
                   : (!waiting && g_xbox_login) ? SCR_LOGIN
#endif
                   : (conn && g_mm.state == MM_ST_LOGIN && g_mm.have_login) ? SCR_LOGIN
                   : waiting ? SCR_WAIT
                   : SCR_LIST;

        b = prev;
        menu_read_buttons(&b);
#define PRESSED(f) (b.f && !prev.f)
        if (screen == SCR_LIST) {
            if (key_repeat(b.up, now, &rep_up))   mm_move(&g_mm, -1, UI_ROWS);
            if (key_repeat(b.down, now, &rep_dn)) mm_move(&g_mm, +1, UI_ROWS);
            if (PRESSED(left))  mm_page(&g_mm, -1, UI_ROWS);
            if (PRESSED(right)) mm_page(&g_mm, +1, UI_ROWS);
            if (PRESSED(lb))    mm_letter(&g_mm, -1, UI_ROWS);
            if (PRESSED(rb))    mm_letter(&g_mm, +1, UI_ROWS);
            if (PRESSED(a)) {
                const mm_game_t *g = mm_current(&g_mm);
                char cmd[16 + MM_ID_LEN];
                if (g) snprintf(cmd, sizeof(cmd), "PLAY\t%s", g->id);
                if (!g) {
                    /* lista vazia: nada */
#ifdef XCLOUD360_LOGIN
                } else if (lt_running()) {
                    /* a thread do login ocupa a hw 3 (o decoder precisa dela) */
                    snprintf(notice, sizeof(notice), "Aguarde: o Xbox ainda esta carregando o catalogo");
#endif
#ifdef XCLOUD360_NATIVE
                } else if (g_native_ok && g_cat_src == 1 && !b.rt) {
                    /* stage 5: A = sessão no próprio Xbox (sem PC). RT+A = PC. */
                    if (!xn_can_play()) {
                        snprintf(notice, sizeof(notice), "Aguarde: encerrando a sessao anterior...");
                    } else {
                        int f2p = (xbox_title_flags(g->id) & XA_F_ADPLAY) != 0;
                        g_xn_seen = 0;
                        stream_set_inject(1);   /* vídeo = WebRTC (já é, desde o boot) */
                        int r = xn_play(g->id, g->name, f2p);
                        g_t_video_first = 0;
                        if (r == XN_OK) {
                            g_native_game = 1;
                            snprintf(play_name, sizeof(play_name), "%s", g->name);
                            printf("[menu] PLAY %s (%s) no Xbox\n", g->id, g->name);
                            waiting = 1;
                            notice[0] = '\0';
                            g_mm.state = MM_ST_NONE;
                            g_mm.state_text[0] = '\0';
                        } else {
                            snprintf(notice, sizeof(notice), "Nao consegui iniciar a sessao no Xbox (%d) - RT+A: via PC", r);
                        }
                    }
#endif
                } else if (conn && !g_stop_pending && ctrl_send_line(cmd) == 0) {
#ifdef XCLOUD360_NATIVE
                    /* fallback: vídeo volta pro TCP :9000 do capture-rtp. Pedido
                     * antes do force_reconnect: a rede aplica os dois em ordem. */
                    stream_set_inject(0);
                    g_native_game = 0;
#endif
                    snprintf(play_name, sizeof(play_name), "%s", g->name);
                    printf("[menu] PLAY %s (%s)\n", g->id, g->name);
                    /* Derruba qualquer conexão de vídeo velha: o jogo novo
                     * entra por uma conexão nova, que começa num IDR. */
                    want_reconn = stream_reconnects_done() + 1;
                    stream_force_reconnect();
                    waiting = 1;
                    notice[0] = '\0';
                    /* STATE do jogo anterior (STREAMING) não vale pro novo
                     * até o PC responder (STARTING/QUEUE/...). */
                    g_mm.state = MM_ST_NONE;
                    g_mm.state_text[0] = '\0';
                } else {
                    snprintf(notice, sizeof(notice), "Sem conexao com o PC - tente de novo");
                }
            }
        } else {
            rep_up = rep_dn = 0;
        }
        if (waiting && (screen == SCR_WAIT || screen == SCR_LOGIN) && PRESSED(b)) {
#ifdef XCLOUD360_NATIVE
            if (g_native_game) {
                /* stage 5: B = DELETE da sessão do Xbox (fila, provisionando,
                 * SDP...). O pedido HTTPS em voo termina antes (até ~20 s);
                 * o próximo PLAY espera a thread da sessão sair. */
                printf("[menu] cancelado, DELETE da sessao do Xbox\n");
                xn_stop();
                g_native_game = 0;
                snprintf(notice, sizeof(notice), "Cancelado: encerrando a sessao do xCloud...");
            } else
#endif
            {
                printf("[menu] cancelado, STOP\n");
                send_stop();
            }
            waiting = 0;
            play_name[0] = '\0';
        }
#ifdef XCLOUD360_LOGIN
        if (!waiting && screen == SCR_LOGIN && g_xbox_login && PRESSED(b)) {
            printf("[menu] login do Xbox pulado\n");
            lt_cancel();
        }
#ifdef XCLOUD360_NATIVE
        if (!waiting && screen == SCR_LIST && PRESSED(y) && xn_busy()) {
            /* login e sessão dividem a hw 3: nunca os dois ao mesmo tempo */
            snprintf(notice, sizeof(notice), "Aguarde: encerrando a sessao anterior...");
        } else
#endif
        if (!waiting && screen == SCR_LIST && PRESSED(y) && !lt_running()) {
            /* Y: refaz o catálogo pelo Xbox (o token salvo vale); no
             * fallback também pede a lista do PC. */
            lt_start();
            if (g_cat_src < 0 && conn) ctrl_send_line("LIST");
            snprintf(notice, sizeof(notice), "Atualizando o catalogo...");
        } else
#endif
        if (!waiting && (screen == SCR_LIST || screen == SCR_LOGIN) && PRESSED(y) && conn) {
            ctrl_send_line("LIST");
            snprintf(notice, sizeof(notice), "Atualizando o catalogo...");
        }
#ifdef XCLOUD360_LOGIN
        if (!waiting && PRESSED(x) && lt_running()) {
            snprintf(notice, sizeof(notice), "Aguarde: o Xbox ainda esta carregando o catalogo");
        } else
#endif
        if (!waiting && PRESSED(x)) {
            printf("[menu] teste offline (stream embutido)\n");
            return MENU_GO_TEST;
        }
#undef PRESSED
        prev = b;
#ifdef XCLOUD360_NATIVE
        if (!waiting && !strncmp(notice, "Cancelado: encerrando", 21) && xn_can_play())
            snprintf(notice, sizeof(notice), "Sessao do xCloud encerrada");
#endif

        menu_draw(screen, conn, play_name, notice);
        udelay(12000);
    }
}

#ifdef XCLOUD360_NATIVE
/* ---- stage 5: plataforma do xc_native no Xbox ---- */
#ifndef XCLOUD360_NATIVE_KBPS
#define XCLOUD360_NATIVE_KBPS 1500
#endif
#ifndef XCLOUD360_SESSION_KEEPALIVE_MS
#define XCLOUD360_SESSION_KEEPALIVE_MS 5000
#endif
#ifndef XCLOUD360_NATIVE_CONSENT_MS
#define XCLOUD360_NATIVE_CONSENT_MS 0
#endif

/* thread de rede: AU -> o mesmo ring que o TCP :9000 enchia */
static int native_inject(const uint8_t *au, size_t n, int idr, int params) {
    (void)idr; (void)params;
    return stream_inject_au(au, n);
}

/* thread de rede: Opus -> mesmo ring do :9004 (produtor único = hw1) */
static void native_audio(uint16_t seq, uint32_t ts, const uint8_t *p, size_t n) {
#ifdef XCLOUD360_AUDIO
    audio_rx_packet(seq, ts, p, (int)n);
#else
    (void)seq; (void)ts; (void)p; (void)n;
#endif
}

/* Qualquer thread: LOG pro PC (UDP :9003). Na tela só os marcos (estado do
 * WebRTC e linhas [xn]); o resto (SDP, ICE, datachannels) só no PC. */
static void native_log(const char *line) {
    np_log_line(line);
    if (!strncmp(line, "[xcw] state", 11) || !strncmp(line, "[xn]", 4)) printf("%s\n", line);
}

/* xcw_log() (libpeer/usrsctp com LOG_REDIRECT): mesmo destino, com teto. */
static void native_lib_log(const char *line) {
    static uint64_t win = 0;
    static int cnt = 0;
    uint64_t t = now_us();
    if (t - win > 1000000) { win = t; cnt = 0; }
    if (++cnt <= 40) np_log_line(line);
}

static void native_before_session(void) { lt_clear_cancel(); }
#endif

/* Boot no modo menu: rede + thread de rede logo de cara (não espera vídeo),
 * depois menu <-> jogo pra sempre. */
static void menu_main(int gpu_on) {
    int net_thread_on = 0;
#ifdef XCLOUD360_LOGIN
    int npr = -1;
#endif
#ifdef XCLOUD360_NATIVE
    int native_inited = 0;
#endif
    printf("[menu] rede: video %s:%d, controle :%d\n", XCLOUD360_STREAM_SERVER_IP,
           XCLOUD360_STREAM_SERVER_PORT, XCLOUD360_CTRL_PORT);
    /* network_init + 1º SYN do vídeo (recusado até haver jogo; ele tenta de
     * novo a cada 500ms sozinho). */
    BOOT_STEP("8 network_init + 1o SYN...");
    stream_init(XCLOUD360_STREAM_SERVER_IP, XCLOUD360_STREAM_SERVER_PORT);
    int net_ok = stream_net_up();
    BOOT_STEP(net_ok ? "9 rede ok" : "9 SEM rede");
    if (net_ok) {
        stream_set_quiet(1);
        /* UDP aqui é seguro: o bug da v0.8.2 era um 2º tcp_pcb, não udp. */
        int ir = input_udp_init(XCLOUD360_STREAM_SERVER_IP, 9001);
        printf("[input-udp] init r=%d\n", ir);
        BOOT_STEP("10 input_udp ok, ctrl_init...");
        ctrl_init(XCLOUD360_STREAM_SERVER_IP, XCLOUD360_CTRL_PORT, XCLOUD360_BUILD_VERSION);
#ifdef XCLOUD360_LOGIN
        /* v0.17: pcbs UDP (DNS, LOG :9003) ainda no main, antes da thread de rede */
        npr = np_init(XCLOUD360_STREAM_SERVER_IP);
        printf("[net] canos HTTPS/DNS r=%d\n", npr);
#endif
#ifdef XCLOUD360_NATIVE
        /* stage 5: WebRTC na thread de rede (antes dela subir: o xn_init roda
         * no main, usrsctp/libsrtp sem threads). O vídeo nasce em modo
         * inject (sem SYN pro :9000); o RT+A volta pro PC. */
        if (npr == 0) {
            static const xn_platform_t plat = { native_inject, native_audio, native_log, native_before_session, NULL };
            xn_cfg_t xc = { XCLOUD360_NATIVE_KBPS, XCLOUD360_SESSION_KEEPALIVE_MS, XCLOUD360_NATIVE_CONSENT_MS, 2 };
            xcw_set_log_hook(native_lib_log);
            xcw_net_set_pump(stream_net_pump);
            if (xn_init(&plat, &xc) == XN_OK) {
                xn_set_load_probe(stream_net_load);
                stream_set_net_extra(xn_net_step);
                stream_set_inject(1);
                native_inited = 1;
            }
            printf("[native] sessao + WebRTC no Xbox: %s (sessao na hw %d, decoder %d threads no jogo)\n",
                   native_inited ? "pronto" : "FALHOU", XST_HW_THREAD, XCLOUD360_NATIVE_DEC_THREADS);
        }
#endif
        input_udp_set_deferred(1);
        g_pad_forward = 0;
        stream_set_idle_hook(net_hook);
        /* Daqui em diante a thread de rede também dá printf ([ctrl], [stream]);
         * console_putch de duas threads ao mesmo tempo embaralha o cursor do
         * console. O hook de log (com lock) aguenta, e o menu mostra o fim
         * dele no rodapé logo em seguida. */
        ui_log_install();
        BOOT_STEP("11 thread de rede...");
        int nr = stream_start_thread(1);
        printf("[net] thread de rede hw=1 r=%d\n", nr);
        BOOT_STEP("12 thread de rede no ar");
        if (nr != 0) {
            input_udp_set_deferred(0);   /* main faz tudo via stream_poll */
        } else {
            g_input_on_net = 1;
            if (!gpu_on) g_split_active = 1;
            net_thread_on = 1;
        }
    } else {
        printf("[menu] sem rede\n");
    }
#ifdef XCLOUD360_NATIVE
    g_native_ok = native_inited && net_thread_on;
    if (native_inited && !net_thread_on) stream_set_inject(0);   /* sem rede própria: só o PC */
#endif
#ifdef XCLOUD360_LOGIN
    /* v0.17: login + catálogo no próprio Xbox (precisa da thread de rede). */
    if (net_thread_on && npr == 0) {
        printf("[login] thread do login hw=%d r=%d\n", LT_HW_THREAD, lt_start());
    } else {
        g_cat_src = -1;
        snprintf(g_xbox_err, sizeof(g_xbox_err), "Xbox: sem thread de rede");
    }
#endif

    avcodec_register_all();
    mm_init(&g_mm);
    char msg[120] = "";
    for (;;) {
        int go = menu_loop(net_ok, msg);
#ifdef XCLOUD360_NATIVE
        int was_native = g_native_game;
#endif
        int r = run_video(go == MENU_GO_STREAM, gpu_on, net_thread_on);
#ifdef XCLOUD360_AUDIO
        char asum[100];
        audio_summary(asum, sizeof(asum));   /* + linha "[audio] sessao" no log */
#endif
#ifdef XCLOUD360_NATIVE
        if (was_native) {
            char ns[110];
            xn_summary(ns, sizeof(ns));
            xc_log("[native] fim do jogo (%d): %s", r, ns);
        }
        if (was_native && r == EXIT_NATIVE_END)
            snprintf(msg, sizeof(msg), "%.*s", (int)sizeof(msg) - 1, g_mm.state == MM_ST_ERROR && g_mm.state_text[0] ? g_mm.state_text
                                                                                         : "A sessao do xCloud terminou");
        else
#endif
        if (r < 0)                  snprintf(msg, sizeof(msg), "Falha ao abrir o decoder (ver log)");
        else if (r == EXIT_PC_IDLE) snprintf(msg, sizeof(msg), "O PC encerrou o jogo");
#ifdef XCLOUD360_AUDIO
        else if (go == MENU_GO_TEST) snprintf(msg, sizeof(msg), "Teste ok | %s", asum);
#else
        else if (go == MENU_GO_TEST) snprintf(msg, sizeof(msg), "Teste offline terminou");
#endif
        else                        msg[0] = '\0';
    }
}
#endif /* XCLOUD360_MENU */

int main(void) {
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    BOOT_STEP("1 xenos+console ok");
    xenon_make_it_faster(XENON_SPEED_FULL);
    BOOT_STEP("2 speed ok");
    xenon_thread_startup();
    BOOT_STEP("3 threads ok");
    usb_init();
    BOOT_STEP("4 usb_init ok");
    /* Warm-up drain: RF unit + wireless controller take ~500-2000ms to
     * enumerate + sync. Without repeated usb_do_poll(), interrupt reports
     * never arrive, ctrl[0] stays zero-init, and controls read as "all
     * released". 200 iters * 10ms = 2s window. */
    for (int i = 0; i < 200; i++) { usb_do_poll(); udelay(10000); }
    fb_direct_init();
    BOOT_STEP("5 usb warmup + fb ok");
#ifdef XCLOUD360_AUDIO
    /* v0.18: só memória (ring, índice do tom). O chip de som e o Opus sobem
     * no 1o PLAY/X (audio_session_begin), não no boot. */
    audio_init();
    BOOT_STEP("6 audio_init ok (sem hardware)");
#endif

    int gpu_on = 0;
#ifdef XCLOUD360_GPU
    /* v0.15: YUV->RGB no Xenos. Tem que vir antes de qualquer thread de
     * hardware (worker, rede, decoder): todas as chamadas Xe_* ficam no main.
     * Se falhar, segue no caminho de CPU (worker + metade de baixo).
     * v0.16 (menu): continua aqui, uma vez por boot; o console fica na tela
     * até o 1º flip, então o menu desenha nele normalmente. */
    printf("[gpu] init Xenos YUV->RGB backend %dx%d...\n", FB_W, FB_H);
    BOOT_STEP("7 gpu init...");
    if (gpu_present_init(FB_W, FB_H) == 0) {
        gpu_on = 1;
    } else {
        printf("[gpu] falling back to CPU convert path (worker hw2 + net-thread bottom half)\n");
    }
#endif

    if (!gpu_on) {
    /* P7.2 worker thread — core 1 primary (thread 2). Stack grows down so
     * pass TOP-address. Leave 128B guard at very top for xenon prologue. */
    for (int i = 0; i < RING_SLOTS; i++) {
        g_ring[i].ready = 0; g_ring[i].done = 0; g_ring[i].cvt_us = 0;
        g_ring[i].bottom_done = 0;
    }
    void *worker_stack_top = g_worker_stack + sizeof(g_worker_stack) - 128;
    int spawn_r = xenon_run_thread_task(2, worker_stack_top, (void*)worker_convert);
    printf("[p7.2] worker spawn thread=2 stack_top=%p r=%d\n",
           worker_stack_top, spawn_r);
    }

    printf("\n\n==== XCloud360 h264play v%s ====\n\n", XCLOUD360_BUILD_VERSION);
    printf("[stream] embedded fallback %zu bytes\n", embedded_stream_len);
    printf("[fb]     direct write @ %p stride=%d px\n", (void*)g_fb, g_fb_stride_pixels);

#ifdef XCLOUD360_MENU
    menu_main(gpu_on);   /* não retorna */
#else
    /* Try TCP first — Aurora-safe (skip network_init if netif already up). */
    printf("[stream] trying TCP %s:%d...\n",
           XCLOUD360_STREAM_SERVER_IP, XCLOUD360_STREAM_SERVER_PORT);
    int net_thread_on = 0;
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
            /* v0.14: daqui pra frente lwIP é só da thread de rede. v0.14.4:
             * hw thread 1 (núcleo 0, irmã do main) em vez da 3 — ela também
             * converte a metade de baixo da imagem, e assim as duas metades
             * ficam em núcleos diferentes. O frame threading do decoder pula
             * a 1 por estar ocupada e usa 4/5/3. */
            input_udp_set_deferred(1);
            /* v0.15: com GPU não há metade de baixo pra converter — sem hook,
             * g_split_active fica 0 e a thread 1 só faz rede. */
            /* v0.15.2: o hook também lê o controle a 1kHz (net_hook). */
            stream_set_idle_hook(net_hook);
            int nr = stream_start_thread(1);
            printf("[net] thread de rede hw=1 r=%d\n", nr);
            if (nr != 0) {
                input_udp_set_deferred(0);
            } else {
                g_input_on_net = 1;
                if (!gpu_on) g_split_active = 1;
                net_thread_on = 1;
            }
        }
    } else {
        printf("[stream] TCP init failed — using embedded\n");
    }

    avcodec_register_all();
    run_video(use_tcp, gpu_on, net_thread_on);

    printf("\n[end] holding — power-cycle to return\n");
    for (;;) {
        if (!g_input_on_net) usb_do_poll();
        udelay(50000);
    }
#endif
    return 0;
}
