/*
 * XCloud360 — h264play: cliente xCloud nativo pro Xbox 360 (libxenon).
 *
 * Boot: rede + thread de rede (hw 1), login Microsoft/Xbox e catálogo no
 * próprio Xbox (source/login, hw 3), depois menu <-> jogo pra sempre.
 * PLAY (A) abre a sessão xCloud pelo Xbox (source/session) e o WebRTC roda
 * na thread de rede (source/webrtc, source/native), alimentando o decoder
 * (frame threading, deps/xenon-pthread) + GPU (YUV->RGB no Xenos), o áudio
 * Opus e o controle.
 *
 * v0.16: menu de jogos; Back+Start 2 s no jogo = volta pro menu.
 * v0.19.3: Back+Start não encerra a sessão (game_session.h): o jogo fica
 * aberto no menu e o A nele volta na hora; outro jogo encerra antes.
 * v0.20.0:
 *   - menu novo (source/ui, MENU-UI.md) como padrão: hero + fileiras de
 *     capas (source/catalog, CATALOG-COVERS.md), botões Reiniciar/Desligar
 *     no topo, telas de estado. O menu de texto (menu_ui.c) saiu;
 *   - o modo PC saiu: nada de launcher (:9002), vídeo TCP :9000, áudio UDP
 *     :9004, controle/KFRQ UDP :9001 nem "B: usar o catálogo do PC". O app
 *     só funciona nativo. tools/xcloud-launcher e tools/xcloud-capture
 *     continuam no repo, mas o app não fala mais com eles;
 * v0.20.1: marca orbita no menu; botão "Atualizar" (source/update,
 *   UPDATE.md): checa sozinho depois do login e instala pelo próprio Xbox.
 * v0.20.1: log/STAT por HTTPS pros testadores beta (source/login/xc_logup.c,
 *   LOGS-UPLOAD.md): só com XC360/BETAKEY.TXT no pendrive; o UDP :9003 pro
 *   PC saiu (era o último resquício do modo PC).
 * v0.20.3 (BETA-GATE.md):
 *   - build BETA=1: chave beta OBRIGATÓRIA e presa ao console (source/beta):
 *     no boot, antes do login, valida XC360/BETAKEY.TXT (ou abre o teclado
 *     na tela); ID de hardware (fusíveis + MAC) no lugar do CONSOLE.ID;
 *     header X-Console em check/logs/update; Configurações no topo;
 *   - atualização obrigatória: logo na entrada (depois da chave, antes do
 *     login) procura versão nova; obrigatória = popup sem "Depois".
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
#include <xenon_smc/xenon_smc.h>   /* v0.19.3: Reiniciar/Desligar no menu */
#include <ppc/timebase.h>
#include <ppc/atomic.h>
#include <time/time.h>
#include <input/input.h>
#include <usb/usbmain.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include "tcp_stream.h"
#ifdef XCLOUD360_GPU
#include "gpu_present.h"   /* v0.15: YUV->RGB no Xenos (make GPU=1) */
#endif
#include "menu_model.h"
#include "game_session.h"   /* v0.19.3: jogo aberto no menu */
#include "polite_wait.h"
#include "ui/xui.h"          /* v0.20.0: menu novo */
#include "ui/xui_mm.h"
#include "ui/xui_cat.h"
#include "ui/xui_fb_xenon.h"
#include "catalog/xc_catalog.h"
#include "login/login_task.h"   /* v0.17: login + catálogo no Xbox */
#include "login/net_pipe.h"
#include "login/xc_logup.h"     /* v0.20.1: log dos testadores beta */
#include "update/xc_update.h"   /* v0.20.1: atualização pelo próprio Xbox */
#include "beta/xc_beta.h"       /* v0.20.3: chave beta presa ao console */
#include "beta/xc_keypad.h"
#include "native/xc_native.h"   /* stage 5: sessão + WebRTC no próprio Xbox */
#include "webrtc/xc_udp_xenon.h"
#include "login/xc_port.h"
#ifdef XCLOUD360_AUDIO
#include "audio/audio.h"        /* v0.18: áudio do jogo (Opus) */
/* v0.18: o teste offline (X) dura pelo menos 2 voltas do tom embutido. */
#define AUDIO_TEST_US 9000000ULL
#endif
/* v0.18.0: cada passo do boot vai pra tela: a foto mostra o último que passou. */
#define BOOT_STEP(s) printf("[boot] %s\n", s)

extern const uint8_t embedded_stream[];
extern const size_t embedded_stream_len;

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.20.0"
#endif
/* v0.20.0: versão curta pra tela e pro log ("v0.20.0"). */
#ifndef XCLOUD360_APP_VERSION
#define XCLOUD360_APP_VERSION "v0.20.0"
#endif
#ifndef XST_HW_THREAD
#define XST_HW_THREAD 3
#endif
#ifndef XCLOUD360_NATIVE_DEC_THREADS
#define XCLOUD360_NATIVE_DEC_THREADS 4   /* v0.19.2: hw 4,2,5,3 (sessão na hw1) */
#endif
#ifndef XCLOUD360_BETA
#define XCLOUD360_BETA 0
#endif
/* v0.20.3: plataforma do xc_beta (beta/xcb_port_xenon.c) */
void xcbp_hw_material(uint64_t fuse[XCB_NFUSE], uint8_t mac[6]);
const xcb_ops_t *xcbp_ops(void);
#define XC360_STR_(x) #x
#define XC360_STR(x) XC360_STR_(x)

#define FB_W 1280
#define FB_H 720

/* ============================================================
 *  P7.2 — caminho de CPU (GPU=0): o main decodifica e copia os planos
 *  pro ring de slots; o worker (hw 2) converte a metade de cima e a
 *  thread de rede (hw 1) a de baixo.
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

/* v0.14.4: convert dividido em duas metades (worker hw 2 + thread de rede). */
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
static void trace_on_gpu_flip(uint64_t seq, uint64_t t_flip_us) {
    int tslot = (int)(seq % TRACE_HISTORY);
    if (g_trace[tslot].au_seq != seq) return;
    g_trace[tslot].t_flipped = t_flip_us;
    g_trace[tslot].flipped_seen = 1;
}
#endif
#endif

/* -------- Direct Xenos tiled framebuffer writer (P7.1, caminho de CPU) ---- */
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

static inline uint32_t fb_tile_off(int x, int y) {
    return ((y >> 5) * 32 * g_fb_stride_pixels
          + ((x >> 5) << 10)
          + (x & 3) + ((y & 1) << 2)
          + (((x & 31) >> 2) << 3)
          + (((y & 31) >> 1) << 6))
          ^ ((y & 8) << 2);
}

static inline uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8);
}

static inline uint8_t clip8(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* BT.601 (video-range), 4 luma por iteração, 2 linhas. */
static void convert_and_present_2rows_fast(
    const uint8_t* Y0, const uint8_t* Y1,
    const uint8_t* U,  const uint8_t* V,
    int W, int y_row)
{
    if (!g_fb) return;
    if ((uint32_t)(y_row + 1) >= g_fb_height) return;
    if ((uint32_t)W > g_fb_width) W = g_fb_width;

    for (int x = 0; x < W; x += 4) {
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
        int idx = g_cons_idx % RING_SLOTS;
        while (!g_ring[idx].ready) {
            if (g_producer_done) {
                if (!g_ring[idx].ready) return;
                break;
            }
            for (volatile int i = 0; i < 100; i++) {}
        }
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
        uint64_t seq = g_ring[idx].au_seq;
        int tslot = (int)(seq % TRACE_HISTORY);
        g_trace[tslot].t_flipped = t1;
        __asm__ __volatile__("eieio" ::: "memory");
        g_trace[tslot].flipped_seen = 1;
#endif
        g_ring[idx].ready = 0;   /* free the slot */
        /* Ordem importa: ready=0 antes de bottom_done=0. */
        __asm__ __volatile__("sync" ::: "memory");
        g_ring[idx].bottom_done = 0;
        g_cons_idx++;
    }
}

/* v0.14.4: hook da thread de rede: metade de baixo do próximo slot. */
static int convert_bottom_step(void) {
    if (!g_split_active) return 0;
    int idx = g_bot_cons_idx % RING_SLOTS;
    if (g_ring[idx].bottom_done) return 0;
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

/* v0.15.1: deadzone radial com rescala (limiar 12000 mata o drift). */
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
/* v0.15.3: stick de câmera: m' = ADZ + (GAIN - ADZ) * m^EXPO. */
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

/* stage 5: 1 = há jogo nativo pedido/em andamento (controle, keyframe e
 * STAT vão pro xc_native). Main escreve; a thread de rede só lê. */
static volatile int g_native_game = 0;
static volatile int g_pad_neutral_req = 0;

/* P11: lê o controle e monta o frame de 24 bytes little-endian (o formato
 * de xc_proto.h) pro xn_pad_frame. v0.19: um frame por controle (até 4). */
#define MAX_PADS 4
static void send_pad(int port) {
    struct controller_data_s d;
    int r = get_controller_data(&d, port);
    static int seen[MAX_PADS];
    if (r && !seen[port]) {
        printf("[input] controller slot %d attached — first fresh report\n", port);
        seen[port] = 1;
    }

    /* Y invertido (libxenon: Y positivo = stick pra cima). Clamp em
     * INT16_MIN pra evitar overflow em -(-32768). */
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
     * foi usado pela primeira vez, não o slot do libxenon. Controle nunca
     * usado não manda nada. */
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
    /* stage 5: direto pro xcw_set_gamepad (esta é a thread de rede, a mesma
     * do WebRTC). v0.20.0: sem jogo nativo o frame não vai pra lugar nenhum. */
    if (g_native_game) xn_pad_frame(buf, 24);
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

/* v0.18.2: junta os 4 slots pro menu e pro Back+Start. */
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

/* v0.15.2: leitura do controle na thread de rede, a 1kHz. Quando
 * g_input_on_net=1 o main não toca mais no USB. */
static volatile int g_input_on_net = 0;
/* v0.16: 0 = lê o USB mas não manda o controle pro jogo (menu). */
static volatile int g_pad_forward = 0;
/* v0.17: a thread do login/catálogo usa o pendrive (FatFs). USB não é
 * thread-safe: enquanto ela segura, a thread de rede não chama usb_do_poll. */
static volatile int g_usb_hold_req = 0, g_usb_hold_ack = 0;
int xc_usb_hold(int on) {
    if (!on) {
        __asm__ __volatile__("sync" ::: "memory");
        g_usb_hold_req = 0;
        __asm__ __volatile__("sync" ::: "memory");
        for (int i = 0; i < 2000 && g_input_on_net && g_usb_hold_ack; i++) udelay(100);
        return 0;
    }
    g_usb_hold_req = 1;
    __asm__ __volatile__("sync" ::: "memory");
    if (!g_input_on_net) return 0;   /* sem thread de rede: o login nem roda */
    for (int i = 0; i < 20000 && !g_usb_hold_ack; i++) udelay(100);
    if (g_usb_hold_ack) return 0;
    g_usb_hold_req = 0;
    __asm__ __volatile__("sync" ::: "memory");
    return -1;
}
/* v0.17.1: hora do último usb_do_poll e o maior buraco entre dois polls. */
static volatile uint64_t g_usb_poll_us = 0;
static volatile uint32_t g_ugap_max_us = 0, g_ugaps = 0;
#define UGAP_SLOW_US 25000
static int net_hook(void) {
    int worked = g_split_active ? convert_bottom_step() : 0;
#ifdef XCLOUD360_AUDIO
    {
        /* v0.18: jitter buffer + Opus + xenon_sound nesta thread, 1x por ms. */
        static uint64_t last_a = 0;
        uint64_t ta = now_us();
        if (ta - last_a >= 1000) {
            last_a = ta;
            worked |= audio_service();
        }
    }
#endif
    if (g_usb_hold_req) {
        g_usb_hold_ack = 1;
        __asm__ __volatile__("sync" ::: "memory");
        return worked;
    }
    if (g_usb_hold_ack) { g_usb_hold_ack = 0; __asm__ __volatile__("sync" ::: "memory"); }
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
    if (g_pad_neutral_req) {   /* saindo do jogo: solta tudo no xCloud */
        g_pad_neutral_req = 0;
        xn_pad_neutral();
    }
    return worked;
}

/* v0.20.0: com o menu novo o console do libxenon não pode desenhar no fb
 * (é o mesmo que o XUI usa): o stdout vai só pra UART (putch). */
static void quiet_stdout(const char *buf, int len) { (void)buf; (void)len; }
extern void (*stdout_hook)(const char *buf, int len);
static void quiet_stdout_install(void) { stdout_hook = quiet_stdout; }

/* AU do ring: [u32 BE n][AU]. v0.11.1: 512KB por AU. */
#define AU_BUF_CAP (512 * 1024)
static uint8_t  g_au_buf[AU_BUF_CAP];
static uint32_t g_pending_au_size = 0;

/* v0.12.0: esperar IDR em vez de decodar lixo (erro de decode ou backlog). */
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
    /* stage 5: PLI + videoKeyframeRequested (o xcw limita a 1/s) */
    if (g_native_game) xn_request_keyframe(0);
}

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

/* No meio do jogo: fecha o decoder e reabre com `want`, esperando o
 * próximo IDR. NULL = não reabriu nem com 1 thread (sai do jogo). */
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
    if (g_pending_au_size == 0) {
        if (stream_available() < 4) return 0;
        uint8_t hdr[4];
        stream_read(hdr, 4);
        g_pending_au_size = ((uint32_t)hdr[0] << 24) |
                            ((uint32_t)hdr[1] << 16) |
                            ((uint32_t)hdr[2] <<  8) |
                            ((uint32_t)hdr[3]);
        if (g_pending_au_size == 0 || g_pending_au_size > AU_BUF_CAP) {
            /* v0.20.0: sem TCP pra derrubar: joga o ring fora (o produtor
             * publica AUs inteiros, o próximo começa num cabeçalho) e pede
             * IDR. Não deveria acontecer com o inject. */
            printf("[play] bogus au_size=%u (cap=%d) — ring descartado, esperando IDR\n",
                   g_pending_au_size, AU_BUF_CAP);
            g_pending_au_size = 0;
            stream_discard();
            g_wait_idr = 1;
            request_idr();
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

/* ============================================================
 *  Menu (v0.16; v0.20.0 = menu novo source/ui)
 *
 *  Donos (quem pode tocar em quê):
 *   - lwIP: só a thread de rede (hw 1). O main só usa APIs com lock/ring.
 *   - USB: só a thread de rede (usb_do_poll a 1kHz no net_hook). O main lê
 *     o estado com get_controller_data(). g_pad_forward = 0 no menu.
 *   - fb: só o main (xui_fb_present copia o retângulo sujo; o stdout vai
 *     só pra UART enquanto o menu está na tela). No jogo a GPU tira o fb
 *     do console da tela no 1º flip.
 *   - hw 3: login (lt_*), catálogo/capas + upload do log (xcc_bg_*) ou a
 *     sessão do xCloud (xn_*), nunca dois ao mesmo tempo. Antes de PLAY,
 *     lt_start ou do teste offline: xcc_bg_stop() e esperar
 *     !xcc_bg_running() (CATALOG-COVERS.md).
 *   - Xe_*: só o main. Xe_Init uma vez no boot.
 * ============================================================ */
#define EXIT_STREAM_END 0    /* stream embutido acabou / erro de decode */
#define EXIT_USER       1    /* Back+Start 2s */
#define EXIT_NATIVE_END 3    /* stage 5: a sessão do Xbox acabou (ERROR/IDLE) */

static menu_model_t g_mm;
static uint64_t     g_hold_since = 0;     /* Back+Start apertados desde */

static int      g_native_ok = 0;   /* xn_init ok + thread de rede de pé */
static uint64_t g_t_video_first = 0;   /* v0.19.2: 1o AU nativo no ring (espera a hw3) */
static unsigned g_xn_seen = 0;
/* Main: linha STATE da sessão do Xbox -> menu_model. 1 se mudou. */
static int native_poll_state(void) {
    char l[240];
    unsigned s = xn_state_line(l, sizeof(l));
    if (!s || s == g_xn_seen) return 0;
    g_xn_seen = s;
    mm_handle_line(&g_mm, l);
    /* v0.20.1: erro de sessão = o lote do log beta sai logo */
    if (g_mm.state == MM_ST_ERROR) xlu_flush_soon();
    return 1;
}

/* ---- v0.19.3: jogo aberto no menu (game_session.h) ---- */
static mm_game_t g_next_play;   /* A num jogo: entra quando a hw 3 / a sessão anterior liberar */
static int       g_next_on = 0;
/* v0.20.4: quem além da sessão pode estar na hw 3 (ver xc_native.c). */
int xc_hw3_other_owner(void) { return xcc_bg_running() || xcu_running() || xcb_running(); }

static int native_paused(void) { return g_native_ok && xn_session(NULL, 0, NULL, 0) == XN_SESS_PAUSED; }

int xc_game_session(xc_game_session_t *o) {
    memset(o, 0, sizeof(*o));
    if (!g_native_ok) return XC_GS_NONE;
    /* xn_session também solta o WebRTC de uma pausada que o xCloud derrubou */
    switch (xn_session(o->id, sizeof(o->id), o->name, sizeof(o->name))) {
        case XN_SESS_STARTING: o->state = XC_GS_STARTING; break;
        case XN_SESS_PLAYING:  o->state = XC_GS_PLAYING; break;
        case XN_SESS_PAUSED:   o->state = XC_GS_OPEN; break;
        case XN_SESS_ENDING:   o->state = XC_GS_ENDING; break;
        default: o->id[0] = o->name[0] = '\0'; break;
    }
    return o->state;
}

/* Zera IDR/AU entre sessões: decoder novo precisa de IDR e os contadores
 * do STAT são por jogo. */
static void video_session_reset(void) {
    g_pending_au_size = 0;
    g_wait_idr = 1;
    g_last_kfrq_us = 0;
    g_skipped_aus = 0;
    g_backlog_events = 0;
    g_decode_errors = 0;
}

/* No jogo, a cada ~10ms: Back+Start por 2s = volta pro menu; a cada 100ms
 * o STATE da sessão do Xbox (ERROR / IDLE = acabou). */
static int ingame_check(uint64_t now) {
    static uint64_t last_st = 0;
    struct controller_data_s d;
    if (!g_input_on_net) usb_do_poll();   /* sem thread de rede o USB é do main */
    get_any_pad(&d);
    if (d.back && d.start) {
        if (!g_hold_since) g_hold_since = now;
        else if (now - g_hold_since >= 2000000) { g_hold_since = 0; return EXIT_USER; }
    } else {
        g_hold_since = 0;
    }
    if (g_native_game && now - last_st >= 100000) {
        last_st = now;
        /* RECONNECTING/STARTING (re-sinalização) ficam no jogo: o vídeo
         * congela e volta sozinho. ERROR / IDLE = acabou. */
        if (native_poll_state() && (g_mm.state == MM_ST_ERROR || g_mm.state == MM_ST_IDLE))
            return EXIT_NATIVE_END;
    }
    return 0;
}

/* Volta do jogo pro menu. Ordem:
 *  1. para de mandar o controle e solta tudo no xCloud; keep = 1
 *     (Back+Start) = xn_pause (sessão segue aberta), senão xn_stop;
 *  2. tela: GPU espera o último frame e devolve o scanout pro fb do console
 *     (gpu_present_finish); no caminho de CPU espera o worker esvaziar;
 *  3. decoder: avcodec_close junta as threads de frame (libera hw 4,2,5,3);
 *  4. ring descartado; zera IDR/contadores. */
static void video_teardown(AVCodecContext *ctx, AVCodecParserContext *parser,
                           AVFrame *frame, int gpu_on, int keep) {
#ifdef XCLOUD360_AUDIO
    audio_session_end();   /* v0.18: fade + silêncio já (menu é mudo) */
#endif
    if (g_native_game) {
        g_pad_forward = 0;
        __asm__ __volatile__("sync" ::: "memory");
        g_pad_neutral_req = 1;
        xc_polite_udelay(20000);   /* hook a 1 kHz zera; o xcw manda em <= 4 ms */
        /* v0.19.3: Back+Start = pausa; sessão que já acabou / erro = DELETE */
        if (!keep || xn_pause() != XN_OK) xn_stop();
    }
    g_pad_forward = 0;
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
            xc_polite_udelay(1000);
        }
    }
    (void)gpu_on;
    quiet_stdout_install();   /* console_open reinstalou o desenho do console */
    if (parser) av_parser_close(parser);
    if (ctx) {
        avcodec_close(ctx);
        av_free(ctx);
    }
    /* v0.19.2: threads de frame juntadas -> a hw 3 pode ir pro DELETE */
    if (g_dec_hw3) { xn_dec_release_hw3(); g_dec_hw3 = 0; }
    xn_set_dec_threads(0);
    if (frame) av_free(frame);
    if (g_native_game) {
        stream_discard();   /* o menu descarta o resto */
        g_native_game = 0;
    }
    video_session_reset();
    xlu_flush_soon();   /* v0.20.1: fim de sessão = lote do log beta */
    printf("[menu] jogo encerrado, decoder fechado\n");
}

/* v0.16: o pipeline de vídeo (decoder, loop de AUs, GPU/worker, STAT).
 * live = 1: jogo nativo (AUs do WebRTC no ring); 0: teste offline (stream
 * embutido + tom). Retorna EXIT_*; -1 = não abriu o decoder. */
static int run_video(int live, int gpu_on, int net_thread_on) {
    int exit_reason = 0;
    (void)net_thread_on;
    uint64_t t_last_chk = 0;
    video_session_reset();
    g_hold_since = 0;
#ifdef XCLOUD360_GPU
    /* 1ª vez: já armado no boot (no-op). Depois de um finish: re-arma. */
    if (gpu_on && gpu_present_init(FB_W, FB_H) != 0) {
        printf("[gpu] re-arm falhou\n");
        if (live) xn_stop();   /* não deixa a sessão aberta pra ninguém */
        g_native_game = 0;
        stream_discard();
        return -1;
    }
#endif
    g_pad_forward = live;   /* teste offline: controle não vai pra lugar nenhum */

    AVCodec* codec = avcodec_find_decoder(CODEC_ID_H264);
    if (!codec) {
        printf("[err] no H264 decoder\n");
        video_teardown(NULL, NULL, NULL, gpu_on, 0);
        return -1;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
#if XCLOUD360_DECODE_THREADS > 1
    /* v0.13: frame threading nas threads de hardware livres. */
    ctx->thread_count = XCLOUD360_DECODE_THREADS;
    {
        /* stage 5: conta as hw threads livres de verdade. v0.19.2: no jogo a
         * sessão sai da hw 3 assim que o answer é publicado, então o decoder
         * volta a ter NATIVE_DEC_THREADS = 4 (hw 4,2,5,3). A hw 3 é ALUGADA
         * (xn_dec_claim_hw3). */
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
    ctx->thread_type  = FF_THREAD_FRAME;
#else
    ctx->thread_count = 1;
    ctx->flags  |= CODEC_FLAG_LOW_DELAY;
#endif
    ctx->flags2 |= CODEC_FLAG2_FAST;
#if XCLOUD360_SKIP_DEBLOCK
    ctx->skip_loop_filter = AVDISCARD_ALL;
#endif
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        printf("[err] avcodec_open2 failed\n");
        video_teardown(ctx, NULL, NULL, gpu_on, 0);
        return -1;
    }
    printf("[dec] threads=%d active_type=%d (1=frame 2=slice) skip_deblock=%d\n",
           ctx->thread_count, ctx->active_thread_type, XCLOUD360_SKIP_DEBLOCK);
    xn_set_dec_threads(ctx->thread_count);
    if (g_native_game) {
        char l[128];
        snprintf(l, sizeof(l), "[dec] jogo nativo: %d threads de frame%s (%s)", ctx->thread_count,
                 g_dec_hw3 ? " (hw 4,2,5,3)" : " (hw3 com a sessao)", XCLOUD360_BUILD_VERSION);
        np_log_line(l);
    }
#ifdef XCLOUD360_AUDIO
    /* v0.18: jogo = Opus do WebRTC; teste offline = tom embutido. */
    audio_session_begin(live ? AUDIO_SRC_RTC : AUDIO_SRC_TEST);
#endif
    AVCodecParserContext* parser = av_parser_init(codec->id);
    AVFrame* frame = avcodec_alloc_frame();
    AVPacket pkt;
    av_init_packet(&pkt);

    printf("[play] mode=%s -> decode -> display\n\n", live ? "WebRTC-live" : "embedded");
#ifdef XCLOUD360_GPU
    if (gpu_on) {
        printf("[play] display = Xenos pixel shader (console goes dark at first frame)\n");
#ifdef XCLOUD360_TRACE_LATENCY
        gpu_present_set_flip_cb(trace_on_gpu_flip);
#endif
    }
#endif

    uint64_t t_wall_start = now_us();
#define REPORT_PERIOD_US 2000000ULL
    uint64_t t_next_report = t_wall_start + REPORT_PERIOD_US;
    uint64_t acc_dec = 0, acc_cvt = 0;
    int frames_since_report = 0;
    int total_frames = 0;
    int prev_total_frames = 0;

    const uint8_t *data = embedded_stream;
    size_t left = embedded_stream_len;

    for (;;) {
        uint8_t *out_data = NULL;
        int out_size = 0;
#ifdef XCLOUD360_TRACE_LATENCY
        uint64_t trace_au_seq = 0;
#endif
        {
            uint64_t tn = now_us();
            if (tn - t_last_chk >= 10000) {
                t_last_chk = tn;
                int r = ingame_check(tn);
                if (r) { exit_reason = r; break; }
                /* v0.19.2: a sessão precisa da hw 3 no meio do jogo:
                 * empresta (decoder com 3) e pega de volta (4). */
                if (g_native_game && live) {
                    int w = xn_dec_threads_wanted(ctx->thread_count, XCLOUD360_NATIVE_DEC_THREADS);
                    if (w != ctx->thread_count) {
                        AVCodecContext *nc = dec_reopen(codec, ctx, w);
                        ctx = nc;
                        if (!ctx) { exit_reason = EXIT_STREAM_END; break; }
                    }
                }
            }
        }
#ifdef XCLOUD360_AUDIO
        if (!g_input_on_net) audio_service();   /* v0.18: sem thread de rede */
#endif
#ifdef XCLOUD360_GPU
        if (gpu_on) gpu_present_poll();
#endif

        if (live) {
            int pulled = pull_next_au(&out_data, &out_size);
            if (pulled <= 0) {
                udelay(1000);
                continue;
            }
#ifdef XCLOUD360_TRACE_LATENCY
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
            /* v0.18: o vídeo embutido recomeça até o tom tocar 2 voltas. */
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
            /* v0.12.0: referência quebrada = espera o próximo IDR. */
            if (live) {
                g_decode_errors++;
                g_wait_idr = 1;
                request_idr();
                continue;
            }
            break;   /* embedded stream: break on decode error */
        }
        if (!got) continue;
#ifdef XCLOUD360_TRACE_LATENCY
        if (live) g_trace[(int)(trace_au_seq % TRACE_HISTORY)].t_decoded = t_b;
#endif

        static int decoded_count = 0;
        decoded_count++;
        acc_dec += (t_b - t_a);
        total_frames++;

#if XCLOUD360_DISPLAY_DIVISOR > 1
        if (decoded_count % XCLOUD360_DISPLAY_DIVISOR) continue;
#endif
        uint64_t t_d;
#ifdef XCLOUD360_GPU
        if (gpu_on) {
            int gW = frame->width  < FB_W ? frame->width  : FB_W;
            int gH = frame->height < FB_H ? frame->height : FB_H;
            uint64_t t_c = now_us();
            uint64_t tag = 0;
#ifdef XCLOUD360_TRACE_LATENCY
            if (live) {
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
            if (live) g_trace[(int)(trace_au_seq % TRACE_HISTORY)].skipped = 1;
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
        if (live) {
            g_ring[prod].au_seq = trace_au_seq;
            g_trace[(int)(trace_au_seq % TRACE_HISTORY)].t_dispatch = now_us();
        }
#endif
        __asm__ __volatile__("eieio; sync" ::: "memory");
        g_ring[prod].ready = 1;
        g_prod_idx++;
        t_d = now_us();

        acc_cvt += (t_d - t_c);
        frames_since_report++;

#ifdef XCLOUD360_GPU
report:
#endif
        if (t_d >= t_next_report) {
            double sec = (t_d - (t_next_report - REPORT_PERIOD_US)) / 1e6;
            int dec_frames = total_frames - prev_total_frames;
            prev_total_frames = total_frames;
            double dec_ms = dec_frames > 0 ? (double)acc_dec / 1000.0 / dec_frames : 0;
            double prod_ms = (double)acc_cvt / 1000.0 / frames_since_report;
            double fps    = frames_since_report / sec;
            double dec_fps = dec_frames / sec;
            int worker_fps_snapshot = g_worker_frames;
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
            if (live && g_native_game) {
                /* STAT do jogo: vai pro log beta (xc_logup) como "STAT ...",
                 * com as chaves do WebRTC no fim e, por último, v0.20.0:
                 * ver=<versão do ELF>. No jogo o worker não roda: as linhas
                 * esperam a volta pro menu. */
                char stat[600];
                int n = snprintf(stat, sizeof(stat),
                    "STAT t=%d dec_fps=%.1f disp_fps=%.1f dec_ms=%.1f prod_ms=%.1f "
                    "cvt_ms=%.1f bot_ms=%.1f ring=%u skipped=%u backlog=%u dec_err=%u",
                    (int)((t_d - t_wall_start)/1000000), dec_fps, fps, dec_ms,
                    prod_ms, wcvt_ms, bot_ms, (unsigned)ring_bytes,
                    g_skipped_aus, g_backlog_events, g_decode_errors);
#ifdef XCLOUD360_GPU
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
                if (n > 0 && n < (int)sizeof(stat)) n += audio_stat(stat + n, sizeof(stat) - n);
#endif
                if (n > 0 && n < (int)sizeof(stat)) n += xn_stat_tail(stat + n, sizeof(stat) - n);
                if (n > 0 && n < (int)sizeof(stat) - 1) {
                    int m = snprintf(stat + n, sizeof(stat) - n, " ver=%s", XCLOUD360_BUILD_VERSION);
                    if (m > 0) n += m;
                    if (n >= (int)sizeof(stat)) n = (int)sizeof(stat) - 1;
                }
                if (n > 0) np_log_line(stat);
            }
            acc_dec = acc_cvt = 0; frames_since_report = 0;
            t_next_report = t_d + REPORT_PERIOD_US;
#ifdef XCLOUD360_TRACE_LATENCY
            uint64_t stop_seq = g_au_seq_counter;
            uint64_t d_ar_de_max = 0, d_ar_de_sum = 0;
            uint64_t d_de_di_max = 0, d_de_di_sum = 0;
            uint64_t d_di_fl_max = 0, d_di_fl_sum = 0;
            uint64_t d_ar_fl_max = 0, d_ar_fl_sum = 0;
            size_t   ring_max = 0; uint64_t ring_sum = 0;
            int n_full = 0, n_skipped = 0, n_pending = 0;
            for (uint64_t s = g_trace_last_reported; s < stop_seq; s++) {
                int idx = (int)(s % TRACE_HISTORY);
                if (g_trace[idx].au_seq != s) continue;
                uint64_t ta = g_trace[idx].t_arrival;
                uint64_t td = g_trace[idx].t_decoded;
                uint64_t tp = g_trace[idx].t_dispatch;
                uint64_t tf = g_trace[idx].t_flipped;
                if (g_trace[idx].skipped) { n_skipped++; continue; }
                if (!tf) { n_pending++; continue; }
                n_full++;
                uint64_t d1 = td - ta, d2 = tp - td, d3 = tf - tp, d4 = tf - ta;
                if (d1 > d_ar_de_max) d_ar_de_max = d1;
                d_ar_de_sum += d1;
                if (d2 > d_de_di_max) d_de_di_max = d2;
                d_de_di_sum += d2;
                if (d3 > d_di_fl_max) d_di_fl_max = d3;
                d_di_fl_sum += d3;
                if (d4 > d_ar_fl_max) d_ar_fl_max = d4;
                d_ar_fl_sum += d4;
                size_t rb = g_trace[idx].ring_bytes_at_arrival;
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
    uint64_t t_wall_end = now_us();
    /* nativo: o video_teardown solta os controles no xCloud e só então faz
     * o xn_pause (Back+Start) ou o xn_stop */
    video_teardown(ctx, parser, frame, gpu_on, exit_reason == EXIT_USER);
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

/* ---- tela do menu (source/ui desenha, menu_model.c guarda o estado) ---- */

#define MENU_GO_STREAM 1
#define MENU_GO_TEST   2

enum { SCR_LIST = 0, SCR_LOGIN, SCR_WAIT, SCR_NONET, SCR_LOADING, SCR_FAIL, SCR_UPDATE,
       SCR_BCHECK, SCR_BKEY, SCR_BMSG, SCR_BSET };   /* v0.20.3: chave beta */

/* v0.20.0: estado do XUI (MENU-UI.md). */
static xui_view_t     *g_xv;
static xui_mm_t        g_xmm;
static xui_cat_t       g_xcat;
static xui_home_data_t g_xd;

/* v0.20.1: atualização (UPDATE.md). g_upd_auto = checar sozinho assim que a
 * hw 3 estiver livre; g_upd_screen = tela de atualização aberta (modal). */
static int        g_upd_auto = 0;
static int        g_upd_screen = 0;
static xcu_snap_t g_us;
/* v0.20.3: atualização obrigatória. g_boot_upd: 1 = procurar antes do login,
 * 2 = procurando; o login espera (g_login_deferred). Falha de rede = segue e
 * tenta de novo em g_upd_retry_us. g_upd_dismissed = versão que o usuário
 * deixou pra depois (só opcional). */
static int        g_boot_upd = 0;
static int        g_login_deferred = 0;
static uint64_t   g_upd_retry_us = 0;
static unsigned   g_upd_seen = 0;
static char       g_upd_dismissed[32] = "";
#define UPD_RETRY_US (10ULL * 60 * 1000000)

/* v0.17: de onde vem o catálogo. 0 = login do Xbox em andamento,
 * 1 = catálogo do Xbox na tela, -1 = login falhou (v0.20.0: sem PC pra cair). */
static int       g_cat_src = 0;
static int       g_xbox_login = 0;       /* tela de código do Xbox */
static unsigned  g_lt_runs_seen = 0;
static lt_snap_t g_lts;
static char      g_xbox_err[170] = "";

/* ---- v0.20.3: chave beta (source/beta, BETA-GATE.md) ----
 * g_bui = tela do beta aberta. Sem autorização (BETA=1) nada além destas
 * telas aparece: nem login, nem capas, nem log. */
enum { BG_NONE = 0, BG_KEY, BG_MSG, BG_SETTINGS };
static int        g_bui = BG_NONE;
static int        g_bchange = 0;          /* trocar chave (veio das Configurações): B volta */
static int        g_beta_boot = 0;        /* falta o xcb_boot_start (hw 3) */
static int        g_beta_pend = 0;        /* 1 = validar g_beta_arg, 2 = ler do pendrive: esperando a hw 3 */
static char       g_beta_arg[16];
static unsigned   g_bseen = 0;
static xcb_snap_t g_bs;
static xkp_t      g_kp;
static char       g_kerr[160];
static char       g_set_notice[64];
static char       g_kp_text[16], g_key_status[80], g_ver_line[64];
static xui_key_t      g_kd;
static xui_settings_t g_sd;

static int beta_ok(void) { return !XCLOUD360_BETA || xcb_authorized(); }
static int beta_screen_p(int scr) { return scr >= SCR_BCHECK && scr <= SCR_BSET; }

/* unix -> "25/10/2026 22:56" no horário de Brasília (UTC-3, sem verão) */
static void fmt_brt(char *o, size_t n, long long t) {
    t -= 3 * 3600;
    long long days = t / 86400, sec = t % 86400;
    if (sec < 0) { sec += 86400; days--; }
    /* civil_from_days (H. Hinnant) */
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(o, n, "%02u/%02u/%04lld %02d:%02d", d, m, y, (int)(sec / 3600), (int)(sec / 60 % 60));
}

/* stage 5: flags do catálogo (XA_F_ADPLAY = jogo com anúncio -> oferta
 * xgpuwebf2p) por titleId; o menu_model só guarda id/nome. */
static struct { char id[MM_ID_LEN]; unsigned flags; } g_xflags[MM_MAX_GAMES];
static int g_nxflags = 0;
static unsigned xbox_title_flags(const char *id) {
    for (int i = 0; i < g_nxflags; i++)
        if (!strcmp(g_xflags[i].id, id)) return g_xflags[i].flags;
    return 0;
}

/* Lista do Xbox -> menu_model, pelas linhas GAME/END. v0.20.0: nomes em
 * UTF-8 (o menu novo desenha acentos). */
static void xbox_catalog_commit(void) {
    xa_title_t *t = NULL;
    int n = 0;
    if (lt_take_games(&t, &n) != 0) return;
    char line[16 + MM_ID_LEN + MM_NAME_LEN];
    g_nxflags = 0;
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "GAME\t%s\t%s\t0", t[i].id, t[i].name[0] ? t[i].name : t[i].id);
        mm_handle_line(&g_mm, line);
        if (g_nxflags < MM_MAX_GAMES) {
            snprintf(g_xflags[g_nxflags].id, MM_ID_LEN, "%.*s", MM_ID_LEN - 1, t[i].id);
            g_xflags[g_nxflags++].flags = t[i].flags;
        }
    }
    snprintf(line, sizeof(line), "END");
    mm_handle_line(&g_mm, line);
    free(t);
}

/* Uma vez por volta do menu. */
static void xbox_login_poll(void) {
    lt_snapshot(&g_lts);
    g_xbox_login = g_lts.phase == LT_RUNNING && g_lts.uri[0];
    if (g_xbox_login) xlu_add_secret(g_lts.code);   /* o código nunca vai pro log */
    if (g_lts.runs == g_lt_runs_seen) return;
    g_lt_runs_seen = g_lts.runs;
    if (g_lts.phase == LT_DONE) {
        xbox_catalog_commit();
        g_cat_src = 1;
        g_xbox_err[0] = 0;
        /* v0.20.3: a atualização já foi procurada antes do login (g_boot_upd) */
        printf("[menu] catalogo: Xbox (%d jogos)\n", g_mm.ngames);
        return;
    }
    if (g_lts.phase == LT_FAILED) {
        snprintf(g_xbox_err, sizeof(g_xbox_err), "%s", g_lts.err);
        xlu_flush_soon();
        /* Já tinha catálogo do Xbox (Y falhou): fica com ele. */
        if (g_cat_src != 1) g_cat_src = -1;
    }
}

typedef struct { int up, down, left, right, a, b, x, y, lb, rb, start; } menu_btn_t;

/* Controle no menu: D-pad ou stick esquerdo. Sem usb_do_poll aqui quando a
 * thread de rede está de pé (ela é a dona do USB). */
static void menu_read_buttons(menu_btn_t *b) {
    struct controller_data_s d;
    if (!g_input_on_net) usb_do_poll();
    else if (now_us() - g_usb_poll_us > 40000) {
        /* v0.17.1: USB parado (pendrive em uso): direções contam como soltas. */
        b->up = b->down = b->left = b->right = 0;
        return;
    }
    get_any_pad(&d);
    b->up    = d.up    || d.s1_y >  20000;   /* libxenon: Y positivo = cima */
    b->down  = d.down  || d.s1_y < -20000;
    b->left  = d.left  || d.s1_x < -20000;
    b->right = d.right || d.s1_x >  20000;
    b->a = d.a; b->b = d.b; b->x = d.x; b->y = d.y; b->lb = d.lb; b->rb = d.rb; b->start = d.start;
}

/* Repetição de tecla segurada: dispara ao apertar, de novo após 400ms e
 * depois a cada 80ms (v0.20.0: 140ms, cada passo anima um card). */
static int key_repeat(int held, uint64_t now, uint64_t *next) {
    if (!held) { *next = 0; return 0; }
    if (*next == 0) { *next = now + 400000; return 1; }
    if (now >= *next) { *next = now + 140000; return 1; }
    return 0;
}

static const char *game_name(const char *id) {
    for (int i = 0; id && i < g_mm.ngames; i++)
        if (!strcmp(g_mm.games[i].id, id)) return g_mm.games[i].name;
    return NULL;
}

/* Desenha um quadro (home ou tela de estado) e copia o sujo pro fb. */
static int menu_present(int screen, const xui_status_t *st, unsigned ms) {
    xui_canvas_t *cv = xui_fb_canvas();
    int anim = 0;
    if (!g_xv || !cv) return 0;
    xui_rect_t r;
    if (screen == SCR_LIST) {
        r = xui_home_frame(g_xv, cv, &g_xd, xui_mm_confirm(&g_xmm, &g_mm), ms, &anim);
    } else if (screen == SCR_BKEY) {
        r = xui_key_frame(g_xv, cv, &g_kd, &g_xd, ms, &anim);   /* v0.20.3 */
    } else if (screen == SCR_BSET) {
        r = xui_settings_frame(g_xv, cv, &g_sd, &g_xd, ms, &anim);
    } else {
        r = xui_status_frame(g_xv, cv, st, &g_xd, ms, &anim);
    }
    xui_fb_present(r);
    if (screen == SCR_LIST) xui_cat_want(&g_xcat, g_xv, &g_xd);
    return anim;
}

/* ---- v0.19.3: Reiniciar / Desligar (v0.20.0: botões do topo) ----
 * A ordem (encerra a sessão, timeout, SMC) está em mm_power_run
 * (menu_model.c, testada no host); aqui só a plataforma: xn_stop = o mesmo
 * DELETE do B e do Back+Start; acabou quando xn_busy() cai. v0.20.0: o
 * worker do catálogo (pendrive!) também para antes do SMC. */
static char g_pw_l1[80], g_pw_l2[160];

static int pw_session_busy(void) {
    return (g_native_ok && xn_busy()) || xcc_bg_running() || xcu_running() || xcb_running();
}

static int pw_session_open(void *u) { (void)u; return pw_session_busy(); }

static void pw_session_stop(void *u) {
    (void)u;
    if (g_native_ok && xn_busy()) {
        printf("[menu] energia: DELETE da sessao do Xbox\n");
        xn_stop();
    }
    g_native_game = 0;
    if (xcc_bg_running()) xcc_bg_stop();
    if (xcu_running()) xcu_cancel();   /* v0.20.1 (a troca de arquivos não cancela: espera o timeout) */
}

static int pw_session_done(void *u) { (void)u; return !pw_session_busy(); }

static unsigned long long pw_now_us(void *u) { (void)u; return now_us(); }

static void pw_draw(void) {
    xui_status_t s;
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_BUSY;
    s.title = g_pw_l1;
    s.text = g_pw_l2;
    s.progress = -1;
    menu_present(SCR_WAIT, &s, (unsigned)(now_us() / 1000));
}

static void pw_wait(void *u) {
    (void)u;
#ifdef XCLOUD360_AUDIO
    if (!g_input_on_net) audio_service();
#endif
    pw_draw();
    xc_polite_udelay(10000);
}

static void pw_show(void *u, const char *l1, const char *l2) {
    (void)u;
    printf("[menu] %s %s\n", l1, l2);
    snprintf(g_pw_l1, sizeof(g_pw_l1), "%s", l1);
    snprintf(g_pw_l2, sizeof(g_pw_l2), "%s", l2);
    pw_draw();
}

static void pw_reboot(void *u) {
    (void)u;
    xc_polite_udelay(300000);   /* dá pra ler o "Reiniciando..." */
    xenon_smc_power_reboot();
    for (;;) xc_polite_udelay(100000);
}

static void pw_shutdown(void *u) {
    (void)u;
    xc_polite_udelay(300000);
    xenon_smc_power_shutdown();
    for (;;) xc_polite_udelay(100000);
}

static void power_action(int item) {
    const mm_power_ops_t ops = { NULL, pw_session_open, pw_session_stop, pw_session_done, pw_now_us,
                                 pw_wait, pw_show, pw_reboot, pw_shutdown };
    printf("[menu] energia: %s\n", item == MM_PWR_REBOOT ? "reiniciar" : "desligar");
    mm_power_run(&ops, item, MM_PWR_TIMEOUT_MS);   /* não volta (SMC) */
}

/* ---- v0.20.3: chave beta ----
 * hw 3 livre pro xc_beta: sem login, sem worker das capas, sem atualização,
 * sem sessão. */
__attribute__((unused)) static int beta_hw3_free(void) {
    return !lt_running() && !xcc_bg_running() && !xcu_running() && !xcb_running() &&
           (!g_native_ok || xn_can_play());
}

static void beta_poll(int *pend_upd) {
    (void)pend_upd;
#if XCLOUD360_BETA
    if (g_beta_boot && beta_hw3_free() && xcb_boot_start() == 0) g_beta_boot = 0;
    if (g_beta_pend && !xcb_running()) {
        if (xcc_bg_running()) xcc_bg_stop();   /* regra da hw 3 */
        else if (beta_hw3_free()) {
            int r = g_beta_pend == 1 ? xcb_submit_start(g_beta_arg) : xcb_file_start();
            if (r == 0) g_beta_pend = 0;
        }
    }
    xcb_snapshot(&g_bs);
    if (g_bs.seq != g_bseen && !g_bs.busy) {
        g_bseen = g_bs.seq;
        switch (g_bs.phase) {
        case XCB_OK:
        case XCB_OFFLINE_OK: {
            char k[72];
            if (xcb_key(k, sizeof(k)) > 0) xlu_use_key(k);   /* log beta com a chave validada */
            memset(k, 0, sizeof(k));
            if (g_bui == BG_KEY || g_bui == BG_MSG) {
                if (g_bchange) {
                    g_bui = BG_SETTINGS;
                    snprintf(g_set_notice, sizeof(g_set_notice), "Chave trocada");
                    /* chave nova pode ter acesso ao canal beta (UPDATE.md passo 7) */
                    if (g_cat_src == 1) *pend_upd = 1;
                } else {
                    g_bui = BG_NONE;
                }
            }
            xkp_clear(&g_kp);
            g_kerr[0] = 0;
            break;
        }
        case XCB_NEED_KEY:
            g_bui = BG_KEY;
            snprintf(g_kerr, sizeof(g_kerr), "%s", g_bs.msg);
            break;
        case XCB_INVALID:
            g_bui = BG_KEY;
            snprintf(g_kerr, sizeof(g_kerr), "Chave inválida. Confira e digite de novo.");
            xkp_clear(&g_kp);
            break;
        case XCB_REVOKED: case XCB_DUPLICATE: case XCB_NOCONN: case XCB_ERROR:
            g_bui = BG_MSG;
            break;
        default:
            break;
        }
    }
#endif
    int son = XCLOUD360_BETA && xcb_authorized();
    if (g_mm.settings_on != son) { g_mm.settings_on = son; mm_rows_fix(&g_mm); }
}

/* Qual tela do beta (ou -1). Sem autorização, sempre alguma. */
static int beta_screen(void) {
#if XCLOUD360_BETA
    int busy = g_bs.busy || g_beta_pend || g_beta_boot;
    if (g_bui == BG_SETTINGS) return SCR_BSET;
    if (g_bui == BG_KEY) return SCR_BKEY;   /* o "Validando…" aparece dentro */
    if (g_bui == BG_MSG) return busy ? SCR_BCHECK : SCR_BMSG;
    if (!xcb_authorized()) return SCR_BCHECK;
#endif
    return -1;
}

/* B/A pedidos na tela da chave */
static void beta_submit_typed(void) {
    if (g_bchange && g_native_ok && !xn_can_play()) {
        snprintf(g_kerr, sizeof(g_kerr), "Feche o jogo aberto antes de trocar a chave.");
        return;
    }
    xkp_text(&g_kp, g_beta_arg);
    g_beta_pend = 1;
    g_kerr[0] = 0;
}

static void beta_from_file(void) {
    if (g_bchange && g_native_ok && !xn_can_play()) {
        snprintf(g_kerr, sizeof(g_kerr), "Feche o jogo aberto antes de trocar a chave.");
        return;
    }
    g_beta_pend = 2;
    g_kerr[0] = 0;
}

static void beta_open_key(int change) {
    g_bui = BG_KEY;
    g_bchange = change;
    xkp_clear(&g_kp);
    g_kerr[0] = 0;
}

/* Loop do menu (main thread). Retorna quando é hora de entrar no pipeline
 * de vídeo: MENU_GO_STREAM (chegou vídeo depois de um PLAY) ou MENU_GO_TEST
 * (X = stream embutido). */
static int menu_loop(int net_ok, const char *msg) {
    quiet_stdout_install();
    if (xui_fb_attach() != 0) printf("[menu] xui_fb_attach falhou\n");
    if (g_xv) xui_view_invalidate(g_xv);

    int waiting = 0;
    int keep_st = XC_GS_NONE;   /* v0.19.3: estado do jogo aberto na volta anterior */
    int pend_relogin = 0, pend_test = 0;   /* v0.20.0: esperando a hw 3 (worker) */
    int pend_upd = 0;   /* v0.20.1: 1 = checar, 2 = instalar, quando a hw 3 largar */
    char upd_title[96], upd_detail[96], upd_label[48];
    uint64_t last_bg_try = 0;
    char play_name[MM_NAME_LEN] = "";
    char play_id[MM_ID_LEN] = "";
    char notice[160];
    char title[120];
    snprintf(notice, sizeof(notice), "%s", msg ? msg : "");
    uint64_t rep_up = 0, rep_dn = 0, rep_lf = 0, rep_rt = 0;
    menu_btn_t prev, b;
    /* Botões já apertados na entrada (Back+Start do jogo, A do teste) não
     * contam como clique. */
    memset(&prev, 0, sizeof(prev));
    menu_read_buttons(&prev);

    for (;;) {
        uint64_t now = now_us();
#ifdef XCLOUD360_AUDIO
        if (!g_input_on_net) audio_service();   /* termina o fade do jogo */
#endif
        xbox_login_poll();
        beta_poll(&pend_upd);   /* v0.20.3 */
        /* v0.20.3: login só depois da chave (BETA) e da checagem de atualização */
        if (g_login_deferred && beta_ok() && !g_boot_upd && !xcu_running() && !pend_upd && !xcb_running() &&
            !g_upd_screen && !lt_running() && !xcc_bg_running()) {
            g_login_deferred = 0;
            printf("[login] thread do login hw=%d r=%d\n", LT_HW_THREAD, lt_start());
        }
        /* sessão encerrando depois do jogo / do B: o STATE dela segue chegando */
        if (!waiting && !g_native_game && g_native_ok && xn_busy()) native_poll_state();
        if (g_native_game && waiting && native_poll_state() &&
            (g_mm.state == MM_ST_ERROR || g_mm.state == MM_ST_IDLE)) {
            /* a sessão do Xbox desistiu ou acabou: volta pra lista com o motivo */
            printf("[menu] sessao do Xbox: %s\n", g_mm.state_text);
            xn_stop();
            g_native_game = 0;
            waiting = 0;
            play_name[0] = play_id[0] = '\0';
            xlu_set_game("");
            snprintf(notice, sizeof(notice), "%.*s", (int)sizeof(notice) - 1,
                     g_mm.state_text[0] ? g_mm.state_text : "Sessão encerrada");
        }
        /* v0.19.3: jogo aberto no menu (xn_session solta o WebRTC se o
         * xCloud derrubou; o STATE ERROR dele já veio pelo native_poll_state) */
        static xc_game_session_t gs;
        int ks = xc_game_session(&gs);
        if (!waiting) {
            if (keep_st == XC_GS_OPEN && ks != XC_GS_OPEN && !g_next_on &&
                (g_mm.state == MM_ST_ERROR || g_mm.state == MM_ST_IDLE) && g_mm.state_text[0]) {
                snprintf(notice, sizeof(notice), "%.*s", (int)sizeof(notice) - 1, g_mm.state_text);
            }
            keep_st = ks;
        } else {
            keep_st = XC_GS_NONE;
        }

        /* Vídeo: fora de um PLAY, o que chegar é sobra — joga fora. */
        if (!waiting) {
            stream_discard();
        } else if (g_native_game && stream_available() >= 4) {
            /* v0.19.2: a thread da sessão entrega as fases ao vivo pra hw1 e
             * sai da hw 3 logo depois do answer; espera ela (até 2 s) pro
             * decoder abrir com as 4 threads. */
            if (!g_t_video_first) g_t_video_first = now;
            if (!(xn_session_thread_busy() && now - g_t_video_first < 2000000)) {
                printf("[menu] video do Xbox chegou (%u B), entrando no jogo%s\n",
                       (unsigned)stream_available(), xn_session_thread_busy() ? " (sessao ainda na hw3)" : "");
                g_t_video_first = 0;
                return MENU_GO_STREAM;
            }
        }

        /* catálogo/capas (MENU-UI.md passo 3) */
        xcc_poll();
        xui_cat_sync(&g_xcat, &g_mm, &g_xd);
        xui_mm_sync(&g_xmm, &g_mm, &g_xd, xui_cat_flags, &g_xcat);
        /* v0.20.1: atualização. Regra da hw 3: só sem login, sem worker do
         * catálogo, sem sessão e fora do jogo (UPDATE.md). */
        xcu_snapshot(&g_us);
        int hw3_ok = !waiting && !g_next_on && !lt_running() && (!g_native_ok || xn_can_play()) && net_ok &&
                     g_input_on_net && !xcb_running() && !g_beta_pend && beta_ok();
        /* v0.20.3: procura na entrada (antes do login) e de novo depois de uma
         * falha de rede; versão nova abre o popup sozinha */
        if (g_boot_upd == 1 && hw3_ok && !pend_upd && !xcu_running()) { pend_upd = 1; g_boot_upd = 2; }
        if (g_upd_retry_us && now >= g_upd_retry_us && hw3_ok && !pend_upd && !xcu_running() && !g_upd_screen) {
            g_upd_retry_us = 0;
            g_upd_auto = 1;
        }
        if (g_upd_auto && !pend_upd && hw3_ok && !xcu_running()) { pend_upd = 1; g_upd_auto = 0; }
        if (g_us.seq != g_upd_seen && !g_us.busy && !pend_upd) {
            g_upd_seen = g_us.seq;
            int ph = g_us.phase;
            if (ph == XCU_AVAILABLE && !g_upd_screen &&
                (g_us.mandatory || strcmp(g_upd_dismissed, g_us.latest))) {
                g_upd_screen = 1;   /* popup: obrigatória (sem Depois) ou opcional nova */
                printf("[update] popup v%s (%s)\n", g_us.latest, g_us.mandatory ? "obrigatoria" : "opcional");
            }
            if (g_boot_upd == 2 && ph != XCU_CHECKING) g_boot_upd = 0;
            /* checagem que falhou (rede/servidor): segue pro menu e tenta depois */
            if (ph == XCU_FAILED && !g_us.latest[0] && !g_upd_screen) g_upd_retry_us = now + UPD_RETRY_US;
        }
        if (pend_upd && hw3_ok) {
            if (xcc_bg_running()) xcc_bg_stop();
            else if (!xcu_running()) {
                int r = pend_upd == 2 ? xcu_install_start() : xcu_check_start();
                printf("[update] %s r=%d\n", pend_upd == 2 ? "instalar" : "procurar", r);
                if (r == 0) g_upd_seen = g_us.seq;               /* v0.20.3: vale o próximo resultado */
                else if (g_boot_upd == 2 && pend_upd == 1) g_boot_upd = 1;   /* hw 3 ocupada: tenta de novo */
                pend_upd = 0;
            }
        }
        /* worker do catálogo (+ upload do log) só com a hw 3 livre */
        if (!waiting && !g_next_on && !pend_relogin && !pend_test && !pend_upd && !xcu_running() && !lt_running() &&
            (!g_native_ok || xn_can_play()) && !xcc_bg_running() && now - last_bg_try >= 1000000 &&
            !xcb_running() && !g_beta_pend && !g_beta_boot && beta_ok() && !g_boot_upd && !g_login_deferred) {
            last_bg_try = now;
            if (net_ok && g_input_on_net) xcc_bg_start();
        }

        int bscr = net_ok ? beta_screen() : -1;
        int screen = !net_ok ? SCR_NONET
                   : bscr >= 0 ? bscr
                   : (g_upd_screen && !waiting) ? SCR_UPDATE   /* v0.20.3: popup antes do login também */
                   : (!waiting && g_xbox_login) ? SCR_LOGIN
                   : waiting ? SCR_WAIT
                   : g_upd_screen ? SCR_UPDATE
                   : (g_cat_src == 0 && g_mm.ngames == 0) ? SCR_LOADING
                   : (g_cat_src < 0 && g_mm.ngames == 0) ? SCR_FAIL
                   : SCR_LIST;

        b = prev;
        menu_read_buttons(&b);
#define PRESSED(f) (b.f && !prev.f)
        /* v0.19.3/v0.20.0: A num jogo -> entra assim que a hw 3 (worker) e a
         * sessão anterior liberarem. O mesmo jogo aberto = volta na hora. */
        if (g_next_on && !waiting && screen == SCR_LIST && !xcc_bg_running() && !lt_running()) {
            const mm_game_t *g = &g_next_play;
            char sid[48];
            int ss = xn_session(sid, sizeof(sid), NULL, 0);
            if (ss == XN_SESS_PAUSED && !strcmp(sid, g->id)) {
                g_next_on = 0;
                if (xn_resume(g->id) == XN_OK) {
                    stream_discard();
                    g_xn_seen = 0;   /* reaplica o STATE atual (STREAMING) */
                    g_t_video_first = 0;
                    g_native_game = 1;
                    snprintf(play_name, sizeof(play_name), "%s", g->name);
                    snprintf(play_id, sizeof(play_id), "%s", g->id);
                    xlu_set_game(g->name);
                    printf("[menu] VOLTA %s: mesma sessao do Xbox, pedindo keyframe\n", g->id);
                    waiting = 1;
                    notice[0] = '\0';
                } else {
                    /* não deu pra retomar: encerra e abre uma nova */
                    printf("[menu] %s: retomada falhou, DELETE e sessao nova\n", g->id);
                    xn_stop();
                    g_next_on = 1;
                }
            } else if (ss == XN_SESS_PAUSED) {
                xn_stop();   /* outro jogo aberto: DELETE antes (já pedido no A; reforço) */
            } else if (xn_can_play()) {
                g_next_on = 0;
                int f2p = (xbox_title_flags(g->id) & XA_F_ADPLAY) != 0;
                g_xn_seen = 0;
                stream_discard();
                int r = xn_play(g->id, g->name, f2p);
                g_t_video_first = 0;
                if (r == XN_OK) {
                    g_native_game = 1;
                    snprintf(play_name, sizeof(play_name), "%s", g->name);
                    snprintf(play_id, sizeof(play_id), "%s", g->id);
                    xlu_set_game(g->name);
                    printf("[menu] PLAY %s no Xbox\n", g->id);
                    xcc_mru_touch(g->id);
                    waiting = 1;
                    notice[0] = '\0';
                    g_mm.state = MM_ST_NONE;
                    g_mm.state_text[0] = '\0';
                } else {
                    snprintf(notice, sizeof(notice), "Não consegui iniciar a sessão no Xbox (%d)", r);
                    xlu_flush_soon();
                }
            }
        }
        if (pend_relogin && !xcc_bg_running()) {
            pend_relogin = 0;
            printf("[menu] Y: login/catalogo de novo (lt_start=%d)\n", lt_start());
        }
        if (pend_test && !xcc_bg_running()) {
            pend_test = 0;
            printf("[menu] teste offline (stream embutido)\n");
            return MENU_GO_TEST;
        }

        if (screen == SCR_LIST) {
            int dx = 0, dy = 0;
            if (key_repeat(b.up, now, &rep_up))    dy = -1;
            if (key_repeat(b.down, now, &rep_dn))  dy = +1;
            if (key_repeat(b.left, now, &rep_lf))  dx = -1;
            if (key_repeat(b.right, now, &rep_rt)) dx = +1;
            if (dx || dy) mm_nav(&g_mm, dx, dy);
            if (PRESSED(lb)) mm_nav_page(&g_mm, -1, 6);
            if (PRESSED(rb)) mm_nav_page(&g_mm, +1, 6);
            if (PRESSED(a)) {
                int act = mm_press_a(&g_mm);
                const mm_game_t *g = mm_focus_game(&g_mm);
                if (act == MM_A_REBOOT || act == MM_A_SHUTDOWN) {
                    power_action(act == MM_A_REBOOT ? MM_PWR_REBOOT : MM_PWR_SHUTDOWN);
                } else if (act == MM_A_SETTINGS) {
                    g_bui = BG_SETTINGS;   /* v0.20.3 */
                    g_bchange = 0;
                    g_set_notice[0] = 0;
                } else if (act == MM_A_UPDATE) {
                    /* v0.20.1: tela de atualização; procura de novo se não há
                     * versão nova conhecida (e nada rodando) */
                    if (g_native_ok && !xn_can_play()) {
                        snprintf(notice, sizeof(notice), "Feche o jogo aberto antes de atualizar");
                    } else {
                        g_upd_screen = 1;
                        g_next_on = 0;
                        int ph = g_us.phase;
                        if (!xcu_running() && ph != XCU_AVAILABLE && ph != XCU_DONE &&
                            !((ph == XCU_FAILED || ph == XCU_CANCELLED) && g_us.latest[0]))
                            pend_upd = 1;
                    }
                } else if (act == MM_A_PLAY && g) {
                    if (xcu_running() || pend_upd) {
                        snprintf(notice, sizeof(notice), "Aguarde: procurando atualização…");
                    } else if (lt_login_running()) {
                        /* a thread do login ocupa a hw 3 (sessão/decoder precisam dela).
                         * v0.20.2: o worker das capas também usa a hw 3, mas ele é
                         * parado logo abaixo; antes o PLAY ficava preso aqui pra sempre. */
                        snprintf(notice, sizeof(notice), "Aguarde: o Xbox ainda está carregando o catálogo");
                    } else if (!g_native_ok || g_cat_src != 1) {
                        snprintf(notice, sizeof(notice), "Sem sessão no Xbox: faça o login (Y: tentar de novo)");
                    } else {
                        char sid[48];
                        int ss = xn_session(sid, sizeof(sid), NULL, 0);
                        g_next_play = *g;
                        g_next_on = 1;
                        if (xcc_bg_running()) xcc_bg_stop();   /* regra da hw 3 */
                        if (ss == XN_SESS_PAUSED && strcmp(sid, g->id)) {
                            printf("[menu] %s aberto: DELETE antes de abrir %s\n", sid, g->id);
                            xn_stop();
                            snprintf(notice, sizeof(notice), "Encerrando a sessão anterior para abrir %.60s… (B: cancelar)",
                                     g->name);
                        } else if (!xn_can_play() && ss != XN_SESS_PAUSED) {
                            snprintf(notice, sizeof(notice), "Aguarde: encerrando a sessão anterior… (B: cancelar)");
                        }
                    }
                }
            }
            if (PRESSED(b) && !mm_press_b(&g_mm) && g_next_on) {
                g_next_on = 0;
                snprintf(notice, sizeof(notice), "Cancelado");
            }
        } else if (screen != SCR_BKEY) {
            rep_up = rep_dn = rep_lf = rep_rt = 0;
        }
        /* ---- v0.20.3: telas da chave beta ---- */
        if (screen == SCR_BKEY && !g_bs.busy && !g_beta_pend) {
            int dx = 0, dy = 0;
            if (key_repeat(b.up, now, &rep_up))    dy = -1;
            if (key_repeat(b.down, now, &rep_dn))  dy = +1;
            if (key_repeat(b.left, now, &rep_lf))  dx = -1;
            if (key_repeat(b.right, now, &rep_rt)) dx = +1;
            if (dx || dy) xkp_nav(&g_kp, dx, dy);
            if (PRESSED(a)) {
                int k = xkp_press(&g_kp);
                if (k == XKP_CONFIRM) beta_submit_typed();
                else if (k == XKP_FROM_FILE) beta_from_file();
                else if (k == XKP_TYPED || k == XKP_DELETED) g_kerr[0] = 0;
            }
            if (PRESSED(start) && xkp_complete(&g_kp)) beta_submit_typed();
            if (PRESSED(y)) beta_from_file();
            if (PRESSED(b) && !xkp_backspace(&g_kp) && g_bchange) g_bui = BG_SETTINGS;
        } else if (screen == SCR_BMSG) {
            int ph = g_bs.phase;
            if (ph == XCB_NOCONN || ph == XCB_ERROR) {
                if (PRESSED(a)) {   /* tentar de novo: a mesma operação */
                    if (g_bs.op == XCB_OP_BOOT) g_beta_boot = 1;
                    else g_beta_pend = g_bs.op == XCB_OP_FILE ? 2 : 1;
                }
                if (PRESSED(y)) beta_open_key(g_bchange);
            } else if (PRESSED(a)) {
                beta_open_key(g_bchange);   /* revogada / bloqueada: só outra chave */
            }
            if (PRESSED(b) && g_bchange) g_bui = BG_SETTINGS;
        } else if (screen == SCR_BSET) {
            if (PRESSED(a)) beta_open_key(1);
            if (PRESSED(b)) { g_bui = BG_NONE; g_bchange = 0; g_set_notice[0] = 0; }
        }
        if (waiting && (screen == SCR_WAIT || screen == SCR_LOGIN) && PRESSED(b)) {
            /* B = DELETE da sessão do Xbox (fila, provisionando, SDP...). */
            printf("[menu] cancelado, DELETE da sessao do Xbox\n");
            xn_stop();
            g_native_game = 0;
            snprintf(notice, sizeof(notice), "Cancelado: encerrando a sessão na nuvem…");
            waiting = 0;
            play_name[0] = play_id[0] = '\0';
            xlu_set_game("");
        }
        if (!waiting && (screen == SCR_LIST || screen == SCR_FAIL) && PRESSED(y)) {
            if (g_native_ok && xn_busy()) {
                /* login e sessão dividem a hw 3: nunca os dois ao mesmo tempo */
                snprintf(notice, sizeof(notice), "%s", native_paused() ? "Há um jogo aberto: encerre-o (outro jogo) para atualizar"
                                                                       : "Aguarde: encerrando a sessão anterior…");
            } else if (xcu_running() || pend_upd) {
                snprintf(notice, sizeof(notice), "Aguarde: procurando atualização…");
            } else if (!lt_login_running() && net_ok && g_input_on_net) {
                /* Y: refaz o catálogo pelo Xbox (o token salvo vale) */
                if (xcc_bg_running()) xcc_bg_stop();
                pend_relogin = 1;
                g_next_on = 0;
                snprintf(notice, sizeof(notice), "Atualizando o catálogo…");
                if (g_cat_src < 0) g_cat_src = 0;
            }
        }
        if (!waiting && PRESSED(x) && screen != SCR_LOGIN && screen != SCR_LOADING && !beta_screen_p(screen) &&
            !(screen == SCR_UPDATE && g_us.mandatory)) {
            if (lt_login_running()) {
                snprintf(notice, sizeof(notice), "Aguarde: o Xbox ainda está carregando o catálogo");
            } else if (xcu_running() || pend_upd || g_upd_screen) {
                /* atualização em andamento: nada de teste offline */
            } else if (g_native_ok && xn_busy()) {
                snprintf(notice, sizeof(notice), "Há uma sessão na nuvem aberta: o teste offline precisa dela fechada");
            } else {
                if (xcc_bg_running()) xcc_bg_stop();
                pend_test = 1;
                g_next_on = 0;
            }
        }
        if (screen == SCR_UPDATE) {
            int ph = g_us.phase;
            if (PRESSED(a)) {
                if (ph == XCU_AVAILABLE || ((ph == XCU_FAILED || ph == XCU_CANCELLED) && g_us.latest[0])) {
                    pend_upd = 2;
                } else if (ph == XCU_FAILED) {
                    pend_upd = 1;
                } else if (ph == XCU_DONE) {
                    power_action(MM_PWR_REBOOT);   /* "Reinicie o Xbox" */
                } else if (ph == XCU_UP_TO_DATE) {
                    g_upd_screen = 0;
                }
            }
            /* v0.20.3: obrigatória (versão nova conhecida) = sem B nenhum */
            int forced = g_us.mandatory && g_us.latest[0] &&
                         (ph == XCU_AVAILABLE || ph == XCU_DOWNLOADING || ph == XCU_VERIFYING || ph == XCU_INSTALLING ||
                          ph == XCU_DONE || ph == XCU_FAILED || ph == XCU_CANCELLED);
            if (PRESSED(b) && !forced) {
                if (xcu_running() && g_us.cancellable) {
                    xcu_cancel();   /* o download retoma da próxima vez */
                    g_upd_screen = 0;
                    pend_upd = 0;
                } else if (!xcu_running() && ph != XCU_INSTALLING) {
                    if (ph == XCU_AVAILABLE) snprintf(g_upd_dismissed, sizeof(g_upd_dismissed), "%s", g_us.latest);
                    g_upd_screen = 0;
                    pend_upd = 0;
                }
            }
        }
#undef PRESSED
        prev = b;
        if (!waiting && !strncmp(notice, "Cancelado: encerrando", 21) && (!g_native_ok || xn_can_play()))
            snprintf(notice, sizeof(notice), "Sessão na nuvem encerrada");

        /* ---- desenho ---- */
        const char *sname = NULL, *sid = NULL;
        if (ks == XC_GS_OPEN && !waiting) {
            sid = gs.id;
            sname = game_name(gs.id);
            if (!sname) sname = gs.name;
        }
        g_xd.session_id = sid;
        g_xd.session_name = sname;
        g_xd.settings_avail = g_mm.settings_on;   /* v0.20.3 */
        g_xd.update_avail = g_us.latest[0] && (g_us.phase == XCU_AVAILABLE || g_us.phase == XCU_FAILED ||
                                               g_us.phase == XCU_CANCELLED);
        if (g_xd.update_avail) {
            snprintf(upd_label, sizeof(upd_label), "Atualizar: v%.24s", g_us.latest);
            g_xd.update_label = upd_label;
        } else {
            g_xd.update_label = NULL;
        }
        const mm_game_t *fg = mm_focus_game(&g_mm);
        g_xd.hint = (sid && fg && !strcmp(fg->id, sid)) ? "A:Voltar ao jogo|Y:Atualizar" : "A:Jogar|Y:Atualizar";
        if (notice[0]) g_xd.notice = notice;
        else if (sid && fg && strcmp(fg->id, sid)) g_xd.notice = "Ao abrir outro jogo, a sessão atual é encerrada.";
        else if (g_cat_src == 1 && g_xbox_err[0]) g_xd.notice = g_xbox_err;
        else if (xc_beta_state() == XC_BETA_INVALID)   /* v0.20.1: a ponte recusou a chave */
            g_xd.notice = "Chave beta inválida: o log não está sendo enviado (até reiniciar).";
        else g_xd.notice = NULL;

        xui_status_t st;
        memset(&st, 0, sizeof(st));
        st.progress = -1;
        switch (screen) {
        case SCR_NONET:
            st.kind = XUI_ST_ERROR;
            st.title = "Sem rede";
            st.text = "A rede do Xbox não subiu (network_init falhou). Verifique o cabo e reinicie o console.";
            st.hint = "X:Teste offline";
            break;
        case SCR_LOGIN:
            st.kind = XUI_ST_LOGIN;
            st.title = "Entre na sua conta Microsoft";
            st.text = "Pra jogar no orbita, use o celular ou o computador e acesse:";   /* v0.20.1 */
            st.url = g_lts.uri;
            st.code = g_lts.code;
            st.detail = g_lts.status[0] ? g_lts.status : "O login fica salvo no pendrive.";
            break;
        case SCR_LOADING:
            st.kind = XUI_ST_LOADING;
            st.title = "Carregando o catálogo";
            st.text = "Entrando na conta e buscando os jogos disponíveis na nuvem.";
            st.detail = g_lts.status[0] ? g_lts.status : NULL;
            break;
        case SCR_FAIL:
            st.kind = XUI_ST_ERROR;
            st.title = "Não foi possível carregar o catálogo";
            st.text = g_xbox_err[0] ? g_xbox_err : "O login não terminou.";
            st.detail = notice[0] ? notice : NULL;
            st.hint = "Y:Tentar de novo|X:Teste offline";
            break;
        case SCR_WAIT:
            st.game_id = play_id[0] ? play_id : NULL;
            st.hint = "B:Cancelar";
            if (g_mm.state == MM_ST_QUEUE) {
                st.kind = XUI_ST_QUEUE;
                st.title = "Na fila";
                st.text = "Os servidores estão cheios. Você entra no jogo assim que chegar a sua vez.";
                st.detail = g_mm.state_text[0] ? g_mm.state_text : NULL;
                st.hint = "B:Sair da fila";
            } else {
                st.kind = XUI_ST_STARTING;
                snprintf(title, sizeof(title), "Iniciando %.80s", play_name[0] ? play_name : "o jogo");
                st.title = title;
                st.text = g_mm.state_text[0] ? g_mm.state_text
                                             : "Preparando o console na nuvem. O vídeo aparece sozinho.";
                st.detail = "No jogo: segure Back + Start por 2 s para voltar ao menu.";
            }
            break;
        case SCR_UPDATE: {
            /* v0.20.1: tela de atualização (UPDATE.md "Menu") */
            int ph = pend_upd == 1 ? XCU_CHECKING : g_us.phase;
            st.kind = XUI_ST_LOADING;
            st.text = g_us.status[0] ? g_us.status : NULL;
            if (ph == XCU_CHECKING || ph == XCU_IDLE) {
                st.title = "Procurando atualização…";
                st.text = "Conferindo se há uma versão nova do orbita.";
                st.hint = "B:Voltar";
            } else if (ph == XCU_UP_TO_DATE) {
                st.kind = XUI_ST_STARTING;
                st.progress = 100;
                snprintf(upd_title, sizeof(upd_title), "Você já está na versão mais nova (v%.24s)", g_us.current);
                st.title = upd_title;
                st.text = "Nenhuma atualização disponível agora.";
                st.hint = "B:Voltar";
            } else if (ph == XCU_AVAILABLE) {
                /* v0.20.3: popup da entrada: versão atual -> nova, notas;
                 * obrigatória = só "Atualizar agora" */
                snprintf(upd_title, sizeof(upd_title), "%s%s", g_us.mandatory ? "Atualização obrigatória"
                                                                             : "Nova versão disponível",
                         strcmp(g_us.channel, "beta") ? "" : " (beta)");
                st.title = upd_title;
                st.text = g_us.notes[0] ? g_us.notes : "Versão nova do orbita.";
                if (g_us.total)
                    snprintf(upd_detail, sizeof(upd_detail), "v%.24s  »  v%.24s  ·  %u KB", g_us.current, g_us.latest,
                             (unsigned)(g_us.total / 1024));
                else
                    snprintf(upd_detail, sizeof(upd_detail), "v%.24s  »  v%.24s", g_us.current, g_us.latest);
                st.detail = upd_detail;
                st.progress = 0;
                st.hint = g_us.mandatory ? "A:Atualizar agora" : "A:Atualizar agora|B:Depois";
            } else if (ph == XCU_DOWNLOADING || ph == XCU_VERIFYING) {
                snprintf(upd_title, sizeof(upd_title), "%s a v%.24s", ph == XCU_DOWNLOADING ? "Baixando" : "Conferindo",
                         g_us.latest);
                st.title = upd_title;
                snprintf(upd_detail, sizeof(upd_detail), "%u de %u KB", (unsigned)(g_us.done / 1024),
                         (unsigned)(g_us.total / 1024));
                st.detail = upd_detail;
                st.progress = g_us.pct;
                st.hint = (g_us.cancellable && !g_us.mandatory) ? "B:Cancelar" : NULL;
            } else if (ph == XCU_INSTALLING) {
                st.title = "Instalando — não desligue o Xbox";
                st.progress = 100;
            } else if (ph == XCU_DONE) {
                st.kind = XUI_ST_STARTING;
                st.progress = 100;
                snprintf(upd_title, sizeof(upd_title), "Atualizado para v%.24s", g_us.latest);
                st.title = upd_title;
                st.text = "Reinicie o Xbox para usar a versão nova.";
                st.hint = g_us.mandatory ? "A:Reiniciar" : "A:Reiniciar|B:Depois";
            } else {   /* FAILED / CANCELLED */
                st.kind = XUI_ST_ERROR;
                st.title = ph == XCU_CANCELLED ? "Atualização cancelada" : "A atualização falhou";
                st.text = g_us.err[0] ? g_us.err : (g_us.status[0] ? g_us.status : "Tente de novo.");
                st.hint = (g_us.mandatory && g_us.latest[0]) ? "A:Tentar de novo" : "A:Tentar de novo|B:Voltar";
            }
            break;
        }
        case SCR_BCHECK: {
            static char det[64];
            snprintf(det, sizeof(det), "ID deste console: %s", xcb_console_short());
            st.kind = XUI_ST_LOADING;
            st.title = "Validando sua chave beta";
            st.text = "Conferindo a chave deste console no servidor do orbita.";
            st.detail = det;
            break;
        }
        case SCR_BMSG: {
            static char det[64];
            snprintf(det, sizeof(det), "ID deste console: %s", xcb_console_short());
            st.kind = XUI_ST_ERROR;
            st.detail = det;
            switch (g_bs.phase) {
            case XCB_REVOKED:
                st.title = "Chave revogada.";
                st.text = "Fale com o administrador.";
                st.hint = g_bchange ? "A:Digitar outra chave|B:Voltar" : "A:Digitar outra chave";
                break;
            case XCB_DUPLICATE:
                st.title = "Chave bloqueada";
                st.text = "Esta chave é exclusiva de um console e foi bloqueada por uso em outro aparelho. "
                          "Fale com o administrador.";
                st.hint = g_bchange ? "A:Digitar outra chave|B:Voltar" : "A:Digitar outra chave";
                break;
            case XCB_NOCONN:
                st.title = "Não foi possível validar sua chave (sem conexão)";
                st.text = "Confira o cabo de rede e tente de novo. Sem conexão o orbita só abre se a chave "
                          "foi validada neste console nos últimos 7 dias.";
                st.hint = g_bchange ? "A:Tentar de novo|Y:Outra chave|B:Voltar" : "A:Tentar de novo|Y:Outra chave";
                break;
            default:
                st.title = "Não foi possível validar sua chave";
                st.text = g_bs.msg[0] ? g_bs.msg : "Tente de novo daqui a pouco.";
                st.hint = g_bchange ? "A:Tentar de novo|Y:Outra chave|B:Voltar" : "A:Tentar de novo|Y:Outra chave";
                break;
            }
            break;
        }
        case SCR_BKEY: {
            int busy = g_bs.busy || g_beta_pend;
            xkp_text(&g_kp, g_kp_text);
            memset(&g_kd, 0, sizeof(g_kd));
            g_kd.title = g_bchange ? "Trocar a chave beta" : "Chave beta";
            g_kd.text = "Pegue sua chave na área do testador em x360.pistatrack.com e digite aqui "
                        "(ela fica presa a este console). Ou grave XC360/BETAKEY.TXT no pendrive pelo PC e aperte Y.";
            g_kd.alphabet = XCB_ALPHABET;
            g_kd.typed = g_kp_text;
            g_kd.len = g_kp.n;
            g_kd.row = g_kp.row;
            g_kd.col = g_kp.col;
            g_kd.console_id = xcb_console_short();
            g_kd.error = g_kerr[0] ? g_kerr : NULL;
            g_kd.busy = busy;
            g_kd.status = g_beta_pend == 2 || (busy && g_bs.op == XCB_OP_FILE) ? "Lendo o pendrive e validando…"
                                                                              : "Validando…";
            g_kd.hint = busy ? NULL
                      : (g_bchange && g_kp.n == 0) ? "A:Digitar|B:Voltar|Y:Ler do pendrive"
                      : "A:Digitar|B:Apagar|Y:Ler do pendrive|Start:Confirmar";
            break;
        }
        case SCR_BSET: {
            memset(&g_sd, 0, sizeof(g_sd));
            if (g_bs.last_ok > 0) {
                char t[32];
                fmt_brt(t, sizeof(t), g_bs.last_ok);
                snprintf(g_key_status, sizeof(g_key_status), "%s em %s", g_bs.phase == XCB_OFFLINE_OK
                         ? "última validação" : "validada", t);
            } else {
                snprintf(g_key_status, sizeof(g_key_status), "validada");
            }
            snprintf(g_ver_line, sizeof(g_ver_line), "orbita %s  ·  versão beta", XCLOUD360_APP_VERSION);
            g_sd.console_short = xcb_console_short();
            g_sd.console_full = xcb_console_id();
            g_sd.key_masked = g_bs.key_masked;
            g_sd.key_status = g_key_status;
            g_sd.version = g_ver_line;
            g_sd.note = "Para reportar um bug, envie um print desta tela junto.";
            g_sd.notice = g_set_notice[0] ? g_set_notice : NULL;
            g_sd.button = "Trocar chave";
            g_sd.hint = "A:Trocar chave|B:Voltar";
            break;
        }
        default:
            break;
        }
        int anim = menu_present(screen, &st, (unsigned)(now / 1000));
        /* animando: ~60 voltas/s; parado: ~12 ms (input + catálogo) */
        xc_polite_udelay(anim ? 2000 : 12000);
    }
}

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

/* thread de rede: AU -> o ring do decoder */
static int native_inject(const uint8_t *au, size_t n, int idr, int params) {
    (void)idr; (void)params;
    return stream_inject_au(au, n);
}

/* thread de rede: Opus -> ring do áudio (produtor único = hw1) */
static void native_audio(uint16_t seq, uint32_t ts, const uint8_t *p, size_t n) {
#ifdef XCLOUD360_AUDIO
    audio_rx_packet(seq, ts, p, (int)n);
#else
    (void)seq; (void)ts; (void)p; (void)n;
#endif
}

/* Qualquer thread: log beta (xc_logup). Na UART só os marcos. */
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

/* Boot: rede + thread de rede logo de cara, depois menu <-> jogo pra sempre. */
static void menu_main(int gpu_on) {
    int net_thread_on = 0;
    int npr = -1;
    int native_inited = 0;
    /* v0.20.1: log beta (junta em RAM; o worker da hw 3 lê a chave e envia) */
    xlu_init(XCLOUD360_APP_VERSION);
    BOOT_STEP("8 network_init...");
    int net_ok = stream_init() == 0 && stream_net_up();
    BOOT_STEP(net_ok ? "9 rede ok" : "9 SEM rede");
    if (net_ok) {
        /* v0.17: pcb UDP do DNS ainda no main, antes da thread de rede */
        npr = np_init();
        printf("[net] canos HTTPS/DNS r=%d\n", npr);
        /* stage 5: WebRTC na thread de rede (antes dela subir: o xn_init roda
         * no main, usrsctp/libsrtp sem threads). */
        if (npr == 0) {
            static const xn_platform_t plat = { native_inject, native_audio, native_log, native_before_session, NULL };
            xn_cfg_t xc = { XCLOUD360_NATIVE_KBPS, XCLOUD360_SESSION_KEEPALIVE_MS, XCLOUD360_NATIVE_CONSENT_MS, 2 };
            xcw_set_log_hook(native_lib_log);
            xcw_net_set_pump(stream_net_pump);
            if (xn_init(&plat, &xc) == XN_OK) {
                xn_set_load_probe(stream_net_load);
                stream_set_net_extra(xn_net_step);
                native_inited = 1;
            }
            printf("[native] sessao + WebRTC no Xbox: %s (sessao na hw %d, decoder %d threads no jogo)\n",
                   native_inited ? "pronto" : "FALHOU", XST_HW_THREAD, XCLOUD360_NATIVE_DEC_THREADS);
        }
        g_pad_forward = 0;
        stream_set_idle_hook(net_hook);
        /* Daqui em diante a thread de rede também dá printf: o console do
         * libxenon não aguenta duas threads; stdout só pra UART. */
        quiet_stdout_install();
        BOOT_STEP("11 thread de rede...");
        int nr = stream_start_thread(1);
        printf("[net] thread de rede hw=1 r=%d\n", nr);
        BOOT_STEP("12 thread de rede no ar");
        if (nr == 0) {
            g_input_on_net = 1;
            if (!gpu_on) g_split_active = 1;
            net_thread_on = 1;
        }
    } else {
        printf("[menu] sem rede\n");
    }
    g_native_ok = native_inited && net_thread_on;
    /* v0.20.3: ID de hardware (fusíveis + MAC; nada disso vai pro log), o
     * mesmo nos logs, no X-Console e na tela. Depois da rede: a config da
     * NAND com o MAC já está em cache. */
    {
        uint64_t fuse[XCB_NFUSE];
        uint8_t mac[6], hk[32];
        char id[33];
        xcbp_hw_material(fuse, mac);
        xcb_hwid(fuse, mac, id, hk);
        memset(fuse, 0, sizeof(fuse));
        memset(mac, 0, sizeof(mac));
        xcb_init(xcbp_ops(), id, hk);
        memset(hk, 0, sizeof(hk));
        xlu_set_console_id(id);
        xcu_set_console_id(id);
        xkp_init(&g_kp);
        xc_log("[beta] console %s%s", xcb_console_short(), XCLOUD360_BETA ? " (build BETA: chave obrigatoria)" : "");
    }
    /* v0.20.0: catálogo/capas: ANTES do lt_start (registra os ganchos do login) */
    xcc_init(NULL);
    xcc_set_worker_extra(xlu_step, xlu_finish);   /* v0.20.1: log beta no worker */
    /* v0.20.1: atualização; a versão tem que bater com a publicada (UPDATE.md) */
    xcu_init(XCLOUD360_BUILD_VERSION, NULL);
    if (net_thread_on && npr == 0) {
        /* v0.20.3: o login sobe no menu_loop depois da chave beta (BETA=1) e
         * da checagem de atualização da entrada */
        g_login_deferred = 1;
        g_boot_upd = 1;
        g_beta_boot = XCLOUD360_BETA;
    } else {
        g_cat_src = -1;
        snprintf(g_xbox_err, sizeof(g_xbox_err), "Sem thread de rede: o login do Xbox não pode rodar.");
    }

    avcodec_register_all();
    mm_init(&g_mm);
    /* v0.20.0: menu novo (~16 MB de caches) */
    g_xv = xui_view_new(FB_W, FB_H);
    if (!g_xv) printf("[menu] xui_view_new: sem memoria (menu sem desenho)\n");
    xui_cat_init(&g_xcat);
    xui_cat_bind(&g_xcat, &g_xd);
    g_xd.version = XCLOUD360_APP_VERSION;
    char msg[160] = "";
    for (;;) {
        int go = menu_loop(net_ok, msg);
        int was_native = g_native_game;
        int r = run_video(go == MENU_GO_STREAM, gpu_on, net_thread_on);
#ifdef XCLOUD360_AUDIO
        char asum[100];
        audio_summary(asum, sizeof(asum));   /* + linha "[audio] sessao" no log */
#endif
        if (was_native) {
            char ns[110];
            xn_summary(ns, sizeof(ns));
            xc_log("[native] fim do jogo (%d): %s", r, ns);
        }
        if (was_native && r == EXIT_NATIVE_END) {
            snprintf(msg, sizeof(msg), "%.*s", (int)sizeof(msg) - 1,
                     g_mm.state == MM_ST_ERROR && g_mm.state_text[0] ? g_mm.state_text : "A sessão na nuvem terminou");
            xlu_set_game("");
        } else if (r < 0) {
            snprintf(msg, sizeof(msg), "Falha ao abrir o decoder (ver log)");
            xlu_set_game("");
        } else if (r == EXIT_USER && go == MENU_GO_STREAM) {
            /* v0.19.3 */
            xc_game_session_t gs;
            if (xc_game_session(&gs) == XC_GS_OPEN) {
                const char *nm = game_name(gs.id);
                snprintf(msg, sizeof(msg), "%.60s segue aberto: escolha de novo para voltar",
                         nm ? nm : (gs.name[0] ? gs.name : gs.id));
            } else {
                msg[0] = '\0';
                xlu_set_game("");
            }
        }
#ifdef XCLOUD360_AUDIO
        else if (go == MENU_GO_TEST) snprintf(msg, sizeof(msg), "Teste ok | %s", asum);
#else
        else if (go == MENU_GO_TEST) snprintf(msg, sizeof(msg), "Teste offline terminou");
#endif
        else {
            msg[0] = '\0';
        }
    }
}

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
     * enumerate + sync. 200 iters * 10ms = 2s window. */
    for (int i = 0; i < 200; i++) { usb_do_poll(); udelay(10000); }
    fb_direct_init();
    BOOT_STEP("5 usb warmup + fb ok");
#ifdef XCLOUD360_AUDIO
    /* v0.18: só memória. O chip de som e o Opus sobem no 1o PLAY/X. */
    audio_init();
    BOOT_STEP("6 audio_init ok (sem hardware)");
#endif

    int gpu_on = 0;
#ifdef XCLOUD360_GPU
    /* v0.15: YUV->RGB no Xenos. Antes de qualquer thread de hardware. */
    printf("[gpu] init Xenos YUV->RGB backend %dx%d...\n", FB_W, FB_H);
    BOOT_STEP("7 gpu init...");
    if (gpu_present_init(FB_W, FB_H) == 0) {
        gpu_on = 1;
    } else {
        printf("[gpu] falling back to CPU convert path (worker hw2 + net-thread bottom half)\n");
    }
#endif

    if (!gpu_on) {
    /* P7.2 worker thread — core 1 primary (thread 2). */
    for (int i = 0; i < RING_SLOTS; i++) {
        g_ring[i].ready = 0; g_ring[i].done = 0; g_ring[i].cvt_us = 0;
        g_ring[i].bottom_done = 0;
    }
    void *worker_stack_top = g_worker_stack + sizeof(g_worker_stack) - 128;
    int spawn_r = xenon_run_thread_task(2, worker_stack_top, (void*)worker_convert);
    printf("[p7.2] worker spawn thread=2 stack_top=%p r=%d\n",
           worker_stack_top, spawn_r);
    }

    printf("\n\n==== orbita v%s (h264play) ====\n\n", XCLOUD360_BUILD_VERSION);   /* v0.20.1: marca */
    printf("[stream] embedded test stream %zu bytes\n", embedded_stream_len);
    printf("[fb]     direct write @ %p stride=%d px\n", (void*)g_fb, g_fb_stride_pixels);

    menu_main(gpu_on);   /* não retorna */
    return 0;
}
