/*
 * orbita — zona morta dos analógicos (v0.21.33, MENU-SETTINGS-PLAN.md §3).
 *
 * C puro (testado no host em hosttest/settings/): padrão, limites, o arquivo
 * XC360/PAD.TXT, a conta (zona morta radial + curva da câmera, a MESMA pro
 * jogo e pra prévia "Jogo recebe" da calibração) e o "Medir sozinho".
 *
 * Global e separada por analógico (D2): vale pra todos os controles. O
 * padrão = os valores de compilação de sempre (STICK_DZ/STICK_OUTER/
 * RSTICK_DZ do Makefile), então quem não mexe não sente diferença.
 *
 * Arquivo (valores brutos 0..32767; a tela mostra %):
 *   XCPAD1
 *   L <zona morta> <limite máximo>
 *   R <zona morta> <limite máximo>
 * Ruim/ausente = padrão. Recuperação: apagar pelo PC, Y na calibração ou
 * LB+RB segurados no boot (a mesma fuga do 720p forçado).
 */
#ifndef XC_PADCFG_H
#define XC_PADCFG_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* v0.15.1/v0.15.3: os mesmos -D do Makefile (padrão) */
#ifndef STICK_DZ
#define STICK_DZ 12000
#endif
#ifndef STICK_OUTER
#define STICK_OUTER 31000
#endif
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

#define XPC_FILE   "PAD.TXT"
#define XPC_FULL   32767
enum { XPC_LEFT = 0, XPC_RIGHT = 1 };

/* limites da tela (%): mínimo 0..60, máximo 60..100, máximo >= mínimo + 10 */
#define XPC_DZ_MIN_PCT   0
#define XPC_DZ_MAX_PCT   60
#define XPC_OUT_MIN_PCT  60
#define XPC_OUT_MAX_PCT  100
#define XPC_GAP_PCT      10

typedef struct {
    int32_t dz[2];      /* zona morta (bruto), [XPC_LEFT], [XPC_RIGHT] */
    int32_t outer[2];   /* limite máximo (bruto) */
} xpc_cfg_t;

void xpc_defaults(xpc_cfg_t *c);
void xpc_default_stick(xpc_cfg_t *c, int stick);
int  xpc_is_default(const xpc_cfg_t *c);
int  xpc_equal(const xpc_cfg_t *a, const xpc_cfg_t *b);

/* % <-> bruto (arredondado; 12000 -> 37%, 31000 -> 95%, 9000 -> 27%) */
int     xpc_pct(int32_t raw);
int32_t xpc_raw(int pct);

/* Prende nos limites (em %; um valor bruto dentro dos limites fica igual).
 * Quem mexeu por último (moved = 0 zona morta, 1 limite) é o que cede pra
 * manter a distância de 10%. */
void xpc_clamp_stick(xpc_cfg_t *c, int stick, int moved);
/* Soma `dpct` (+1/-1) na zona morta (what = 0) ou no limite (what = 1),
 * em passos inteiros de %, já prendendo. 1 = mudou. */
int  xpc_step(xpc_cfg_t *c, int stick, int what, int dpct);

/* Arquivo. parse: 0 = ok (cfg preenchida e presa nos limites), -1 = ruim
 * (cfg = padrão). format: bytes escritos (sem o NUL) ou -1. */
int  xpc_parse(const char *s, size_t n, xpc_cfg_t *c);
int  xpc_format(const xpc_cfg_t *c, char *out, size_t cap);

/* A conta do jogo (v0.15.1 apply_deadzone + v0.15.3 apply_curve, iguais):
 * radial com rescala; no direito, depois a curva da câmera. Entrada no
 * sentido do xCloud (Y positivo = cima depois da inversão do send_pad). */
void xpc_deadzone(int16_t *x, int16_t *y, float dz, float outer);
void xpc_curve(int16_t *x, int16_t *y);
void xpc_apply(int stick, int32_t dz, int32_t outer, int16_t *x, int16_t *y);

/* Prévia da calibração: posição e o que o jogo recebe, em % (0..100+). */
int  xpc_mag_pct(int x, int y);
int  xpc_out_pct(int stick, int32_t dz, int32_t outer, int x, int y);

/* ---- Medir sozinho ----
 * (1) "Solte o analógico" 2 s: guarda o maior raio em repouso r0; mínimo =
 *     r0 * 1,25 + 2% (entre 5% e 60%).
 * (2) "Gire o analógico encostando na borda" até 5 s: maior raio em 16
 *     setores; com >= 12 setores (raio >= 50%), máximo = menor desses * 0,97
 *     (entre 70% e 100%); senão o máximo fica como estava.
 * Depois mínimo/máximo passam pelo xpc_clamp_stick. */
enum { XPC_M_IDLE = 0, XPC_M_REST, XPC_M_SPIN, XPC_M_DONE };
#define XPC_M_REST_MS 2000
#define XPC_M_SPIN_MS 5000
#define XPC_M_SECTORS 16
#define XPC_M_NEED    12
typedef struct {
    int      stage;
    uint32_t t0;
    int      r0;                    /* maior raio em repouso (% x 100) */
    int      sect[XPC_M_SECTORS];   /* maior raio por setor (% x 100) */
    int      dz_pct, out_pct;       /* resultado (DONE) */
    int      spun;                  /* 1 = achou setores suficientes */
} xpc_meas_t;
void xpc_meas_start(xpc_meas_t *m, uint32_t now_ms);
/* x/y brutos (qualquer sentido). Devolve o estágio. Ao virar DONE, aplica
 * em c[stick] (out_pct só se spun). */
int  xpc_meas_feed(xpc_meas_t *m, uint32_t now_ms, int x, int y, xpc_cfg_t *c, int stick);
int  xpc_meas_sectors(const xpc_meas_t *m);
/* segundos que faltam no estágio atual (pra tela) */
int  xpc_meas_left_s(const xpc_meas_t *m, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
