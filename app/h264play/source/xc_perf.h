/*
 * orbita — Desempenho (v0.21.43): temperaturas do SMC, carga da CPU/GPU,
 * FPS e o custo dos serviços, pra seção Configurações > Desempenho e pro
 * overlay no canto do jogo.
 *
 * Só o main thread chama (o SMC e o XUI não têm lock). As temperaturas são
 * lidas no máximo 1x por segundo; carga/FPS vêm do laço do jogo (a cada STAT).
 */
#ifndef XC_PERF_H
#define XC_PERF_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cpu_c10, gpu_c10;      /* temperatura em décimos de °C (-1 = sem leitura) */
    int cpu_pct, gpu_pct;      /* carga % (-1 = sem dados: fora do jogo ainda) */
    int fps10;                 /* FPS mostrado x10 (-1 = sem jogo) */
    int audio_ms10;            /* ms/s gastos no áudio (x10) */
    int pad_ms100;             /* ms/s no controle (x100) */
    int net_pct;               /* % da thread de rede (hw1) */
    int dec_ms10;              /* ms por quadro no decoder (x10) */
    int in_game;               /* 1 = números ao vivo do jogo; 0 = da última partida */
} xpf_t;

extern xpf_t xpf;
extern int   xpf_overlay;       /* 1 = números no canto do jogo (XC360/PERF.TXT) */
extern volatile int xpf_rtt_ms10;   /* ida e volta do vídeo com o xCloud (WebRTC), x10; -1 = sem */

/* main: lê o SMC se passou 1 s (now_us). */
void xpf_temps_poll(uint64_t now_us);
/* jogo: a cada STAT (dt em us desde o anterior) */
void xpf_game_sample(double fps, double dec_ms, unsigned gpu_busy_us, uint64_t dt_us);
void xpf_game_end(void);
/* texto do overlay (3 linhas, \n) */
void xpf_overlay_text(char *o, size_t n);

#ifdef __cplusplus
}
#endif
#endif
