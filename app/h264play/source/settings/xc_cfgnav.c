/*
 * orbita — navegação das Configurações (v0.21.33). Ver xc_cfgnav.h.
 */
#include "xc_cfgnav.h"

#include <string.h>

void xcf_init(xcf_nav_t *n) {
    memset(n, 0, sizeof(*n));
    n->sec = XCF_SEC_ACCOUNT;
}

void xcf_open(xcf_nav_t *n) {
    n->open = 1;
    if (n->sec < 0 || n->sec >= XCF_NSEC) n->sec = XCF_SEC_ACCOUNT;
    n->item = n->sec;
    n->right = 0;
    n->page = XCF_PG_MAIN;
    n->confirm = XCF_CF_NONE;
    n->confirm_yes = 0;
    n->changes++;
}

void xcf_close(xcf_nav_t *n) {
    n->open = 0;
    n->right = 0;
    n->page = XCF_PG_MAIN;
    n->confirm = XCF_CF_NONE;
    n->confirm_yes = 0;
    n->changes++;
}

void xcf_set_options(xcf_nav_t *n, int sec, int count, unsigned adj) {
    if (sec < 0 || sec >= XCF_NSEC) return;
    if (count < 0) count = 0;
    if (count > XCF_MAX_OPT) count = XCF_MAX_OPT;
    if (n->nopt[sec] != count || n->adj[sec] != adj) n->changes++;
    n->nopt[sec] = count;
    n->adj[sec] = adj;
    if (n->focus[sec] >= count) n->focus[sec] = count > 0 ? count - 1 : 0;
    if (n->focus[sec] < 0) n->focus[sec] = 0;
    /* a seção aberta perdeu as opções (ex. o cabo pegou DHCP): foco volta pra coluna */
    if (n->sec == sec && n->right && n->page == XCF_PG_MAIN && count == 0) {
        n->right = 0;
        n->item = sec;
    }
}

void xcf_set_page(xcf_nav_t *n, int page) {
    n->page = page;
    if (page != XCF_PG_MAIN) n->right = 1;
    n->changes++;
}

void xcf_ask(xcf_nav_t *n, int kind) {
    n->confirm = kind;
    n->confirm_yes = 0;   /* A sem querer não desliga */
    n->changes++;
}

static xcf_ev_t ev(int act, const xcf_nav_t *n) {
    xcf_ev_t e;
    memset(&e, 0, sizeof(e));
    e.act = act;
    e.sec = n->sec;
    e.opt = n->focus[n->sec];
    e.page = n->page;
    return e;
}

/* LB/RB: seção anterior/seguinte, foco na coluna (sai de subpágina) */
static xcf_ev_t sec_step(xcf_nav_t *n, int d) {
    int s = (n->item >= XCF_NSEC ? n->sec : n->item) + d;
    if (s < 0) s = 0;
    if (s >= XCF_NSEC) s = XCF_NSEC - 1;
    int from = n->page;
    n->sec = s;
    n->item = s;
    n->right = 0;
    n->page = XCF_PG_MAIN;
    n->changes++;
    xcf_ev_t e = ev(from != XCF_PG_MAIN ? XCF_A_PAGE_BACK : XCF_A_MOVED, n);
    e.page = from;
    return e;
}

xcf_ev_t xcf_key(xcf_nav_t *n, int key) {
    xcf_ev_t none = ev(XCF_A_NONE, n);
    if (!n->open) return none;
    /* confirmação: só esquerda/direita, A e B (Guia não faz nada) */
    if (n->confirm) {
        if (key == XCF_K_LEFT || key == XCF_K_RIGHT) {
            n->confirm_yes = key == XCF_K_RIGHT;
            n->changes++;
            return ev(XCF_A_MOVED, n);
        }
        if (key == XCF_K_A || key == XCF_K_B) {
            int kind = n->confirm, yes = key == XCF_K_A && n->confirm_yes;
            n->confirm = XCF_CF_NONE;
            n->confirm_yes = 0;
            n->changes++;
            if (!yes) return ev(XCF_A_MOVED, n);
            return ev(kind == XCF_CF_REBOOT ? XCF_A_REBOOT : kind == XCF_CF_SHUTDOWN ? XCF_A_SHUTDOWN : XCF_A_PADRESET, n);
        }
        return none;
    }
    if (key == XCF_K_GUIDE) {
        xcf_ev_t e = ev(XCF_A_CLOSE, n);
        xcf_close(n);
        return e;
    }
    /* subpáginas */
    if (n->page != XCF_PG_MAIN) {
        if (key == XCF_K_B || (n->page == XCF_PG_NOTES && key == XCF_K_LEFT)) {
            xcf_ev_t e = ev(XCF_A_PAGE_BACK, n);
            n->page = XCF_PG_MAIN;
            n->right = 1;
            n->changes++;
            return e;
        }
        if (n->page == XCF_PG_NOTES && (key == XCF_K_LB || key == XCF_K_RB)) return sec_step(n, key == XCF_K_LB ? -1 : 1);
        if (n->page == XCF_PG_CALIB && (key == XCF_K_LB || key == XCF_K_RB)) return none;   /* sair só pelo B */
        xcf_ev_t e = ev(XCF_A_PAGE, n);
        e.key = key;
        return e;
    }
    if (key == XCF_K_LB || key == XCF_K_RB) return sec_step(n, key == XCF_K_LB ? -1 : 1);
    if (!n->right) {
        switch (key) {
        case XCF_K_UP:
        case XCF_K_DOWN: {
            int it = n->item + (key == XCF_K_UP ? -1 : 1);
            if (it < 0) it = 0;
            if (it >= XCF_NITEMS) it = XCF_NITEMS - 1;
            if (it == n->item) return none;
            n->item = it;
            if (it < XCF_NSEC) n->sec = it;   /* o painel mostra a seção na hora */
            n->changes++;
            return ev(XCF_A_MOVED, n);
        }
        case XCF_K_RIGHT:
        case XCF_K_A:
            if (n->item >= XCF_NSEC) {
                if (key != XCF_K_A) return none;
                xcf_ask(n, n->item == XCF_IT_REBOOT ? XCF_CF_REBOOT : XCF_CF_SHUTDOWN);
                return ev(XCF_A_MOVED, n);
            }
            if (n->nopt[n->sec] <= 0) return none;   /* Rede com cabo: nada pra focar */
            n->right = 1;
            n->focus[n->sec] = 0;                    /* entra na 1ª opção */
            n->changes++;
            return ev(XCF_A_MOVED, n);
        case XCF_K_B: {
            xcf_ev_t e = ev(XCF_A_CLOSE, n);
            xcf_close(n);
            return e;
        }
        default:
            return none;
        }
    }
    /* painel */
    int *f = &n->focus[n->sec];
    int adj = (n->adj[n->sec] >> *f) & 1;
    switch (key) {
    case XCF_K_UP:
    case XCF_K_DOWN: {
        int g = *f + (key == XCF_K_UP ? -1 : 1);
        if (g < 0) g = 0;
        if (g >= n->nopt[n->sec]) g = n->nopt[n->sec] - 1;
        if (g < 0 || g == *f) return none;
        *f = g;
        n->changes++;
        return ev(XCF_A_MOVED, n);
    }
    case XCF_K_LEFT:
    case XCF_K_RIGHT:
        if (adj) {
            xcf_ev_t e = ev(XCF_A_ADJUST, n);
            e.dir = key == XCF_K_LEFT ? -1 : 1;
            return e;
        }
        if (key == XCF_K_RIGHT) return none;
        /* esquerda = volta pra coluna, como o B */
        /* fallthrough */
    case XCF_K_B:
        n->right = 0;
        n->item = n->sec;
        n->changes++;
        return ev(XCF_A_MOVED, n);
    case XCF_K_A:
        return ev(XCF_A_OPTION, n);
    default:
        return none;
    }
}
