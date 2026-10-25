/*
 * XCloud360 stage 3 — máquina de estados da sessão xCloud no Xbox.
 *
 * Quem roda o quê:
 *   - thread da sessão (hw 3, xs_task.c): xsm_step() em loop. Cada passo faz
 *     NO MÁXIMO um pedido HTTPS (bloqueante, pelo net_pipe) e devolve quantos
 *     ms esperar até o próximo.
 *   - thread de rede (hw 1, onde o WebRTC/libpeer vai morar): NUNCA faz
 *     HTTPS (o pipe é servido por ela mesma -> travaria). Conversa com a
 *     sessão só pela "caixa" xsm_rtc_* abaixo (lock + cópia, sem bloquear).
 *   - main (hw 0, menu): xsm_snapshot() -> linha "STATE\t<CODE>\t<texto>" que
 *     o menu_model já entende (mesmos códigos/textos do launcher do PC).
 *
 * Fluxo (igual ao capture-rtp.cpp + engine.cpp do green-nx):
 *   AUTH (refresh->XSTS->gssv) -> ACTIVE (sessão velha: cancela/retoma/pergunta)
 *   -> START (/play) -> POLL (/state 1x/s; /connect no ReadyToConnect; fila)
 *   -> WAIT_OFFER (WebRTC entrega o offer já com munging) -> SDP (/sdp POST +
 *   GET até 60s) -> ICE_BRIDGE (manda locais, espera >=1 remoto até 8s, e só
 *   então publica answer+remotos = ordem "pre-SRD" do capture-rtp) ->
 *   CONNECTING (ICE contínuo + keepalive 5s; morto se ICE ok e DTLS mudo 12s)
 *   -> STREAM_WAIT (DTLS ok, esperando vídeo) -> STREAMING.
 *   Falha numa tentativa: DELETE, 3s de cooldown, sessão nova (2 tentativas).
 *   Queda no meio do jogo: re-sinaliza a MESMA sessão (até 3 em 2 min), senão
 *   sessão nova.
 *
 * v0.19.2 — quem DIRIGE a máquina (cfg.net_live = 1, o jogo nativo):
 *   - até o answer publicado (AUTH..ICE_BRIDGE) a thread da sessão (hw 3),
 *     bloqueante, como antes (o decoder ainda não abriu);
 *   - nas fases ao vivo (CONNECTING, STREAM_WAIT, STREAMING) a thread entrega
 *     a máquina pra thread de REDE (owner = NET) e SAI, liberando a hw 3 pro
 *     decoder. A rede chama xsm_net_step() a cada passo: keepalive, ICE e os
 *     eventos do WebRTC, tudo não-bloqueante pela conexão keep-alive (só a
 *     cifra dos registros). Nada que gaste CPU de verdade ou espere roda lá:
 *     re-login (401), handshake TLS completo, re-sinalização, falha de
 *     tentativa e o DELETE do fim devolvem a máquina (owner = WANT_THREAD)
 *     e a thread da sessão sobe de novo na hw 3 só pra isso (xc_native.c
 *     decide quando a hw 3 está livre), entregando de volta quando a fase
 *     volta a ser ao vivo.
 */
#ifndef XC360_XS_MACHINE_H
#define XC360_XS_MACHINE_H
#include "xc_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* O que fazer com uma sessão já ativa na conta (o servidor só aceita uma). */
enum {
    XSM_ACTIVE_CANCEL = 0,   /* padrão: DELETE em todas (= cleanup_stale_sessions) */
    XSM_ACTIVE_RESUME = 1,   /* mesma titleId: retoma; outras: DELETE */
    XSM_ACTIVE_ASK = 2       /* para em XSM_P_ASK até xsm_answer_active() */
};

typedef enum {
    XSM_P_IDLE = 0, XSM_P_AUTH, XSM_P_ACTIVE, XSM_P_ASK, XSM_P_START, XSM_P_POLL,
    XSM_P_WAIT_OFFER, XSM_P_SDP, XSM_P_ICE_BRIDGE, XSM_P_CONNECTING, XSM_P_STREAM_WAIT,
    XSM_P_STREAMING, XSM_P_COOLDOWN, XSM_P_STOPPING, XSM_P_DONE, XSM_P_ERROR
} xsm_phase_t;

/* Eventos que o lado WebRTC informa (xsm_rtc_report). */
enum {
    XSM_RTC_ICE_UP = 1,      /* ICE CONNECTED/COMPLETED (começa a janela de 12s do DTLS) */
    XSM_RTC_MEDIA_UP = 2,    /* DTLS+SCTP ok (PEER_CONNECTION_COMPLETED) */
    XSM_RTC_VIDEO = 3,       /* primeiro AU de vídeo decodificável -> STREAMING */
    XSM_RTC_DEAD = 4,        /* PC FAILED/CLOSED antes de subir: sessão nova */
    XSM_RTC_DROPPED = 5      /* caiu no meio do jogo: re-sinaliza a mesma sessão */
};

typedef struct {
    char title_id[48];
    char name[64];            /* ASCII, pra tela */
    int  f2p;                 /* 1 = xgpuwebf2p (jogos com anúncio, XA_F_ADPLAY) */
    int  active_policy;       /* XSM_ACTIVE_* */
    int  max_attempts;        /* 2 (capture-rtp MAX_SESSION_ATTEMPTS) */
    int  poll_ms;             /* 1000 (/state) */
    int  queue_timeout_s;     /* 1800 (WaitingForResources, engine.cpp ~30 min) */
    int  state_timeout_s;     /* 210 (outros estados, engine.cpp 300x0.7s) */
    int  resume_timeout_s;    /* 21 (re-sinalização, engine.cpp 30x0.7s) */
    int  offer_timeout_s;     /* 30 (WebRTC tem que entregar o offer) */
    int  sdp_timeout_s;       /* 60 (exchange_sdp: 120x500ms) */
    int  sdp_poll_ms;         /* 500 */
    int  pre_srd_ms;          /* 8000 (capture-rtp: 40x200ms) */
    int  ice_poll_ms;         /* 200 (capture-rtp) */
    int  keepalive_ms;        /* 5000 (capture-rtp; green-nx usa 15000) */
    int  dtls_wait_s;         /* 12 (dead-media window) */
    int  media_deadline_s;    /* 17 (DTLS_WAIT_SECS + 5, depois do answer) */
    int  first_video_s;       /* 30 (DTLS ok mas sem vídeo -> sessão nova) */
    int  cooldown_ms;         /* 3000 (libera o slot da conta) */
    int  waittime;            /* 1 = GET /v1/waittime na fila (ETA pra tela) */
    int  max_resignals;       /* 3 em 2 min (engine.cpp #61) */
    int  net_live;            /* v0.19.2: 1 = fases ao vivo na thread de rede */
} xsm_cfg_t;

void xsm_cfg_defaults(xsm_cfg_t *c, const char *title_id, const char *name);

#define XSM_MAX_LOCAL  16
#define XSM_MAX_REMOTE 32

/* v0.19.2: dono da máquina (xs_lock). */
enum { XSM_OWN_THREAD = 0, XSM_OWN_NET = 1, XSM_OWN_WANT_THREAD = 2, XSM_OWN_DONE = 3 };

typedef struct {
    /* config + sessão (só o DONO mexe: thread da sessão ou, nas fases ao
     * vivo com net_live, a thread de rede) */
    xsm_cfg_t    cfg;
    xs_creds_t   creds;
    xs_session_t s;
    xsm_phase_t  phase;
    int          attempt;          /* 1..max_attempts */
    int          resumed;          /* sessão retomada/re-sinalizada (cap curto no POLL) */
    int          connected;        /* /connect já mandado nesta sessão */
    xs_sstate_t  last_ss;
    uint64_t     t_phase, t_state, t_next_ka, t_next_wt, t_media, t_ice_up, t_video;
    int          net_errs;
    int          eta_s;            /* do /v1/waittime (-1) */
    int          resignals;
    uint64_t     resignal_window;
    char        *answer_tmp;       /* answer recebido, ainda não publicado (pre-SRD) */
    char        *offer_sent;       /* cópia do offer (candidatos de fallback) */
    char         ufrag[64];
    int          locals_sent;
    int          remote_end;
    int          ask_pending;
    xs_active_t  active[XS_MAX_ACTIVE];
    int          nactive;
    char         last_err[160];
    int          has_token;        /* refresh já veio do chamador */
    /* v0.19.2: lado da rede (só com owner = NET) */
    uint64_t     t_net_next;       /* próximo pedido não antes disso */
    uint64_t     t_aq;             /* início do pedido assíncrono em voo */
    int          aq_then_ms;       /* espera depois que ele terminar */
    int          aq_total;         /* ICE_SEND: nlocal no envio */
    unsigned     handoffs, escalations;
    char         esc_why[96];

    /* ---- caixa compartilhada (xs_lock) ---- */
    unsigned     gen;              /* muda a cada PC novo necessário */
    int          want_offer;
    char        *offer;            /* entregue pelo WebRTC */
    char         locals[XSM_MAX_LOCAL][XS_CAND_LEN];
    int          nlocal;
    char        *answer;           /* publicado pra o WebRTC (pre-SRD feito) */
    int          answer_taken;
    char         remotes[XSM_MAX_REMOTE][XS_CAND_LEN];
    int          nremote, remote_taken;
    int          ev_ice_up, ev_media_up, ev_video, ev_dead, ev_dropped;
    int          stop_req;
    int          ask_answer;       /* -1 = sem resposta, 0 = cancelar, 1 = retomar */
    int          owner;            /* v0.19.2: XSM_OWN_* */
    char         code[16];
    char         text[160];
    unsigned     seq;
} xsm_t;

/* Thread da sessão. `refresh` pode ser NULL (lê TOKEN.TXT). */
void xsm_init(xsm_t *m, const xsm_cfg_t *cfg, const char *refresh);
/* Um passo. >= 0: ms até o próximo passo; -1: terminou (DONE/ERROR). */
int  xsm_step(xsm_t *m);
void xsm_free(xsm_t *m);

/* Qualquer thread (lock). */
void xsm_request_stop(xsm_t *m);
void xsm_answer_active(xsm_t *m, int resume);
/* Copia CODE/texto; devolve seq (muda a cada atualização). */
unsigned xsm_snapshot(xsm_t *m, char *code, size_t ccap, char *text, size_t tcap);
/* "STATE\tCODE\ttexto" pro mm_handle_line(). */
unsigned xsm_state_line(xsm_t *m, char *out, size_t cap);
xsm_phase_t xsm_phase(xsm_t *m);

/* ---- lado WebRTC (thread de rede; nunca bloqueiam) ----
 * gen: muda quando é preciso um PeerConnection NOVO (tentativa nova ou
 * re-sinalização). O WebRTC derruba o PC velho, cria outro e manda o offer
 * com a gen atual; chamadas com gen velha são ignoradas. */
unsigned xsm_rtc_gen(xsm_t *m);
int  xsm_rtc_want_offer(xsm_t *m, unsigned *gen);
/* Offer COM o munging já aplicado (ver SESSION-STAGE3.md). Copiado. */
void xsm_rtc_set_offer(xsm_t *m, unsigned gen, const char *sdp);
/* Candidato local cru do libpeer ("candidate:..." ou "a=candidate:..."). */
void xsm_rtc_add_local(xsm_t *m, unsigned gen, const char *cand);
/* Answer (malloc, o chamador dá free) quando pronto; NULL antes. */
char *xsm_rtc_take_answer(xsm_t *m, unsigned gen);
/* Copia até `max` remotos novos (depois do answer). Retorna quantos. */
int  xsm_rtc_take_remote(xsm_t *m, unsigned gen, char (*out)[XS_CAND_LEN], int max);
void xsm_rtc_report(xsm_t *m, unsigned gen, int ev);

const char *xsm_phase_name(xsm_phase_t p);

/* ---- v0.19.2 ---- */
/* Corpo da thread da sessão (xs_task.c e o gêmeo do host): xsm_step em
 * fatias de 50 ms até DONE/ERROR (1: credenciais liberadas, owner = DONE)
 * ou, com cfg.net_live, até uma fase ao vivo (0: owner = NET, a thread
 * deve sair sem tocar mais na máquina). */
int  xsm_thread_run(xsm_t *m);
/* (a peça do xsm_thread_run que entrega: 1 = owner virou NET) */
int  xsm_try_handoff(xsm_t *m, uint64_t t0_ms);
/* Thread de rede, só com owner == NET. Nunca bloqueia. 0 = segue com a
 * rede; 1 = precisa da thread da sessão (owner virou WANT_THREAD, motivo em
 * m->esc_why). */
int  xsm_net_step(xsm_t *m);
int  xsm_owner(xsm_t *m);
void xsm_set_owner(xsm_t *m, int owner);
int  xsm_live_phase(xsm_phase_t p);

#ifdef __cplusplus
}
#endif
#endif
