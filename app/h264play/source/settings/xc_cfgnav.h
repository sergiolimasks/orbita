/*
 * orbita — navegação das Configurações em menu lateral (v0.21.33,
 * MENU-SETTINGS-PLAN.md §2 "Navegação").
 *
 * C puro, como o menu_model (testado no host em hosttest/settings/): coluna
 * de seções (+ ENERGIA: Reiniciar/Desligar), painel de opções, subpáginas
 * (Novidades, Calibração), diálogo de confirmação. Quem diz quantas opções
 * cada seção tem agora (e quais aceitam esquerda/direita, como o Volume) é o
 * main (xcf_set_options, a cada volta). Só o main thread mexe nele.
 *
 * Botões (convenção do usuário): A = confirma/ativa; B = volta (painel ->
 * coluna, subpágina -> seção, calibração = salva e volta) ou, na coluna,
 * fecha; Guia fecha tudo (menos com uma confirmação aberta). X nunca.
 */
#ifndef XC_CFGNAV_H
#define XC_CFGNAV_H

#ifdef __cplusplus
extern "C" {
#endif

enum { XCF_SEC_ACCOUNT = 0, XCF_SEC_NET, XCF_SEC_AV, XCF_SEC_PADS, XCF_NSEC };
/* itens da coluna: as 4 seções + energia */
enum { XCF_IT_REBOOT = XCF_NSEC, XCF_IT_SHUTDOWN, XCF_NITEMS };
enum { XCF_PG_MAIN = 0, XCF_PG_NOTES, XCF_PG_CALIB };
enum { XCF_CF_NONE = 0, XCF_CF_REBOOT, XCF_CF_SHUTDOWN, XCF_CF_PADRESET };

/* teclas (bordas de subida; repetição de direcional é do main) */
enum { XCF_K_UP = 0, XCF_K_DOWN, XCF_K_LEFT, XCF_K_RIGHT, XCF_K_A, XCF_K_B, XCF_K_Y, XCF_K_LB, XCF_K_RB, XCF_K_GUIDE };

/* o que o main tem que fazer */
enum {
    XCF_A_NONE = 0,
    XCF_A_MOVED,      /* só a tela mudou */
    XCF_A_CLOSE,      /* fechar tudo (B na coluna, Guia); ev.page = página de onde saiu */
    XCF_A_OPTION,     /* A na opção ev.opt da seção ev.sec */
    XCF_A_ADJUST,     /* esquerda/direita (ev.dir) numa opção ajustável */
    XCF_A_REBOOT,     /* confirmado */
    XCF_A_SHUTDOWN,
    XCF_A_PADRESET,
    XCF_A_PAGE,       /* tecla ev.key pra subpágina ev.page (rolar notas, calibração) */
    XCF_A_PAGE_BACK   /* B na subpágina ev.page: voltou pra seção */
};

typedef struct { int act, sec, opt, dir, key, page; } xcf_ev_t;

#define XCF_MAX_OPT 8

typedef struct {
    int      open;
    int      item;                 /* foco na coluna (0..XCF_NITEMS-1) */
    int      sec;                  /* seção mostrada no painel */
    int      right;                /* 1 = foco no painel */
    int      page;                 /* XCF_PG_* */
    int      focus[XCF_NSEC];      /* opção em foco por seção */
    int      nopt[XCF_NSEC];
    unsigned adj[XCF_NSEC];        /* bit i = opção i aceita esquerda/direita */
    int      confirm, confirm_yes; /* XCF_CF_*; foco 0 = Cancelar (padrão) */
    unsigned changes;              /* ++ a cada mudança (redesenho) */
} xcf_nav_t;

void xcf_init(xcf_nav_t *n);
/* Abre na última seção aberta nesta sessão (a 1ª vez: Conta e sobre), foco na coluna. */
void xcf_open(xcf_nav_t *n);
void xcf_close(xcf_nav_t *n);
/* Opções focáveis da seção agora; prende o foco (0 opções = foco volta pra coluna). */
void xcf_set_options(xcf_nav_t *n, int sec, int count, unsigned adj_mask);
void xcf_set_page(xcf_nav_t *n, int page);
void xcf_ask(xcf_nav_t *n, int confirm_kind);
xcf_ev_t xcf_key(xcf_nav_t *n, int key);

#ifdef __cplusplus
}
#endif
#endif
