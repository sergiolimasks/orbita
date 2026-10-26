/*
 * XCloud360 v0.19.3 — "tem jogo aberto?" pro menu.
 *
 * Desde a v0.19.3 Back+Start (2 s) volta pro menu SEM encerrar a sessão do
 * xCloud: o keepalive continua, o vídeo/áudio que chega é jogado fora no
 * receptor (nada é decodificado, nenhum keyframe é pedido) e os controles
 * ficam neutros no jogo. No menu:
 *   - A no MESMO jogo: volta na hora (sem fila, sem /play), pede keyframe;
 *   - A em OUTRO jogo: encerra a sessão aberta (DELETE / STOP) e só então
 *     abre a nova;
 *   - Reiniciar/Desligar (mm_power_run): encerra antes, como sempre;
 *   - se o xCloud derrubar a sessão no menu (inatividade, erro), o estado
 *     volta sozinho pra XC_GS_NONE e o A abre uma sessão nova.
 * Vale para o jogo nativo (sessão + WebRTC no Xbox, xc_native). v0.20.0:
 * o modo ponte-PC (launcher.py, PAUSE no :9002) saiu do app.
 *
 * Implementado em main.c. Só a thread principal (a do menu) chama. Barato
 * (um lock, sem rede): pode chamar a cada quadro do menu, e é o que mantém
 * o estado limpo quando o xCloud derruba uma sessão nativa pausada.
 *
 * Uso típico no desenho da lista:
 *     xc_game_session_t gs;
 *     if (xc_game_session(&gs) == XC_GS_OPEN && !strcmp(gs.id, g->id))
 *         ... marca "em andamento" nesse jogo ...
 */
#ifndef XCLOUD360_GAME_SESSION_H
#define XCLOUD360_GAME_SESSION_H

enum {
    XC_GS_NONE = 0,   /* nenhuma sessão: A abre uma nova */
    XC_GS_STARTING,   /* PLAY pedido, subindo (tela de espera; só nativo) */
    XC_GS_PLAYING,    /* no jogo (só nativo; no menu não aparece) */
    XC_GS_OPEN,       /* aberta e pausada no menu: A nesse jogo volta na hora */
    XC_GS_ENDING      /* encerrando (DELETE em curso; só nativo) */
};

typedef struct {
    int  state;       /* XC_GS_* */
    char id[48];      /* titleId (mm_game_t.id), "" com XC_GS_NONE */
    char name[64];    /* nome pra tela */
} xc_game_session_t;

/* Preenche *out e devolve out->state. */
int xc_game_session(xc_game_session_t *out);

#endif
