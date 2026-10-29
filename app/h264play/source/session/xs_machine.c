/* XCloud360 stage 3 — ver xs_machine.h. Lógica de tentativas/tempos portada
 * de capture-rtp.cpp main() e green-nx/src/switch/stream/engine.cpp (GPL-3.0). */
#include "xs_machine.h"
#include "xs_port.h"
#include "../login/xc_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_phase[] = {
    "IDLE", "AUTH", "ACTIVE", "ASK", "START", "POLL", "WAIT_OFFER", "SDP", "ICE_BRIDGE",
    "CONNECTING", "STREAM_WAIT", "STREAMING", "COOLDOWN", "STOPPING", "DONE", "ERROR"
};

const char *xsm_phase_name(xsm_phase_t p) {
    return (unsigned)p < sizeof(k_phase) / sizeof(k_phase[0]) ? k_phase[p] : "?";
}

void xsm_cfg_defaults(xsm_cfg_t *c, const char *title_id, const char *name) {
    memset(c, 0, sizeof(*c));
    snprintf(c->title_id, sizeof(c->title_id), "%s", title_id ? title_id : "");
    snprintf(c->name, sizeof(c->name), "%s", name && name[0] ? name : c->title_id);
    c->active_policy = XSM_ACTIVE_CANCEL;
    c->max_attempts = 2;
    c->poll_ms = 1000;
    c->queue_timeout_s = 1800;
    c->state_timeout_s = 210;
    c->resume_timeout_s = 21;
    c->offer_timeout_s = 30;
    c->sdp_timeout_s = 60;
    c->sdp_poll_ms = 500;
    c->pre_srd_ms = 8000;
    c->ice_poll_ms = 200;
    c->keepalive_ms = 5000;
    c->dtls_wait_s = 12;
    c->media_deadline_s = 17;
    c->first_video_s = 30;
    c->video_total_s = 75;
    c->cooldown_ms = 3000;
    c->waittime = 1;
    c->max_resignals = 3;
    c->net_live = 0;
}

/* ------------------------------------------------------------ UI / estado */

static void ui(xsm_t *m, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ui(xsm_t *m, const char *code, const char *fmt, ...) {
    char t[sizeof(m->text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    for (char *p = t; *p; p++) if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
    int changed;
    xs_lock();
    changed = strcmp(m->code, code) != 0 || strcmp(m->text, t) != 0;
    if (changed) {
        snprintf(m->code, sizeof(m->code), "%s", code);
        snprintf(m->text, sizeof(m->text), "%s", t);
        m->seq++;
    }
    xs_unlock();
    if (changed) xc_log("[sess] STATE %s %s", code, t);
}

/* STARTING vira RECONNECTING "Reconectando: ..." durante re-sinalização
 * (= _phase() do launcher). */
static void ui_phase(xsm_t *m, const char *text) {
    if (m->resignals > 0 && m->resumed) ui(m, "RECONNECTING", "Reconectando: %s", text);
    else ui(m, "STARTING", "%s", text);
}

static void go(xsm_t *m, xsm_phase_t p) {
    if (m->phase != p) xc_log("[sess] fase %s -> %s", xsm_phase_name(m->phase), xsm_phase_name(p));
    m->phase = p;
    m->t_phase = xcn_now_ms();
}

/* Sessão/PC novos: zera a caixa e muda a geração (o WebRTC derruba o PC). */
static void new_gen(xsm_t *m) {
    free(m->answer_tmp);
    m->answer_tmp = NULL;
    free(m->offer_sent);
    m->offer_sent = NULL;
    m->ufrag[0] = 0;
    m->locals_sent = 0;
    m->remote_end = 0;
    m->t_ice_up = 0;
    xs_lock();
    m->gen++;
    m->want_offer = 0;
    free(m->offer);
    m->offer = NULL;
    free(m->answer);
    m->answer = NULL;
    m->answer_taken = 0;
    m->nlocal = m->nremote = m->remote_taken = 0;
    m->ev_ice_up = m->ev_media_up = m->ev_video = m->ev_dead = m->ev_dropped = 0;
    xs_unlock();
}

static void fail(xsm_t *m, const char *msg) {
    xc_log("[sess] ERRO: %s", msg);
    snprintf(m->last_err, sizeof(m->last_err), "%s", msg);
    xs_stop(&m->s);
    new_gen(m);
    ui(m, "ERROR", "%s", msg);
    go(m, XSM_P_ERROR);
}

/* Falha desta tentativa: DELETE e sessão nova depois do cooldown, ou ERROR
 * se acabaram as tentativas (capture-rtp: 2). */
static void attempt_fail(xsm_t *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void attempt_fail(xsm_t *m, const char *fmt, ...) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    xs_lock();
    int keep = m->keep;
    xs_unlock();
    if (m->attempt >= m->cfg.max_attempts || keep) { fail(m, msg); return; }   /* v0.19.3: pausado = sem sessão nova */
    xc_log("[sess] tentativa %d/%d falhou: %s", m->attempt, m->cfg.max_attempts, msg);
    snprintf(m->last_err, sizeof(m->last_err), "%s", msg);
    xs_stop(&m->s);
    new_gen(m);
    ui_phase(m, "Tentando de novo...");
    go(m, XSM_P_COOLDOWN);
}

static void login_fail(xsm_t *m, int r, const xc_err_t *e) {
    char msg[160];
    if (r == XS_ELOGIN) snprintf(msg, sizeof(msg), "Login expirou: entre de novo no Xbox");
    else snprintf(msg, sizeof(msg), "Login falhou: %.140s", e->msg[0] ? e->msg : "erro");
    fail(m, msg);
}

/* v0.21.34: o "<1 min" que aparecia sempre estava errado (o xCloud manda 0
 * ou um número curto enquanto a fila anda) e o pessoal cancelava achando que
 * tinha travado. Agora: há quanto tempo está na fila (sempre verdade),
 * posição se vier, e estimativa só quando o servidor der >= 1 min. */
static void queue_text(char *out, size_t cap, int pos, int eta, int waited_s) {
    char parts[96] = "";
    int n = 0;
    if (waited_s >= 60) n += snprintf(parts + n, sizeof(parts) - (size_t)n, "há %d min", waited_s / 60);
    else n += snprintf(parts + n, sizeof(parts) - (size_t)n, "há %d s", waited_s < 0 ? 0 : waited_s);
    if (pos > 0) n += snprintf(parts + n, sizeof(parts) - (size_t)n, " · posição %d", pos);
    if (eta >= 60) snprintf(parts + n, sizeof(parts) - (size_t)n, " · estimativa ~%d min", (eta + 59) / 60);
    snprintf(out, cap, "Na fila do xCloud %s. Aguarde, o jogo abre sozinho.", parts);
}

/* ------------------------------------------------------------------ init */

void xsm_init(xsm_t *m, const xsm_cfg_t *cfg, const char *refresh) {
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    if (cfg->home) m->cfg.waittime = 0;
    if (m->cfg.max_attempts < 1) m->cfg.max_attempts = 1;
    xs_creds_init(&m->creds, cfg->home ? "xhome" : cfg->f2p ? "xgpuwebf2p" : "xgpuweb");
    if (refresh && refresh[0]) m->has_token = xs_creds_set_refresh(&m->creds, refresh) == XS_OK;
    xs_init(&m->s, &m->creds);
    m->phase = XSM_P_IDLE;
    m->eta_s = -1;
    m->ask_answer = -1;
    m->owner = XSM_OWN_THREAD;
    m->t_phase = xcn_now_ms();
    snprintf(m->code, sizeof(m->code), "STARTING");
    snprintf(m->text, sizeof(m->text), "Iniciando %s...", m->cfg.name);
    m->seq = 1;
}

void xsm_free(xsm_t *m) {
    xs_creds_free(&m->creds);
    free(m->answer_tmp);
    free(m->offer_sent);
    free(m->offer);
    free(m->answer);
    m->answer_tmp = m->offer_sent = m->offer = m->answer = NULL;
}

/* ------------------------------------------------------- caixa (WebRTC) */

void xsm_request_stop(xsm_t *m) { xs_lock(); m->stop_req = 1; xs_unlock(); }

void xsm_set_keep(xsm_t *m, int on) { xs_lock(); m->keep = on ? 1 : 0; xs_unlock(); }

void xsm_answer_active(xsm_t *m, int resume) { xs_lock(); m->ask_answer = resume ? 1 : 0; xs_unlock(); }

unsigned xsm_snapshot(xsm_t *m, char *code, size_t ccap, char *text, size_t tcap) {
    xs_lock();
    if (code && ccap) snprintf(code, ccap, "%s", m->code);
    if (text && tcap) snprintf(text, tcap, "%s", m->text);
    unsigned s = m->seq;
    xs_unlock();
    return s;
}

unsigned xsm_state_line(xsm_t *m, char *out, size_t cap) {
    char c[16], t[160];
    unsigned s = xsm_snapshot(m, c, sizeof(c), t, sizeof(t));
    snprintf(out, cap, "STATE\t%s\t%s", c, t);
    return s;
}

xsm_phase_t xsm_phase(xsm_t *m) { return m->phase; }

unsigned xsm_rtc_gen(xsm_t *m) { xs_lock(); unsigned g = m->gen; xs_unlock(); return g; }

int xsm_rtc_want_offer(xsm_t *m, unsigned *gen) {
    xs_lock();
    int w = m->want_offer && !m->offer;
    if (gen) *gen = m->gen;
    xs_unlock();
    return w;
}

void xsm_rtc_set_offer(xsm_t *m, unsigned gen, const char *sdp) {
    size_t n = strlen(sdp) + 1;
    char *c = (char *)malloc(n);
    if (!c) return;
    memcpy(c, sdp, n);
    xs_lock();
    if (gen == m->gen && m->want_offer && !m->offer) { m->offer = c; c = NULL; }
    xs_unlock();
    free(c);
}

void xsm_rtc_add_local(xsm_t *m, unsigned gen, const char *cand) {
    char t[XS_CAND_LEN];
    size_t l = strcspn(cand, "\r\n");
    if (l == 0 || l >= sizeof(t)) return;
    memcpy(t, cand, l);
    t[l] = 0;
    xs_lock();
    if (gen == m->gen && m->nlocal < XSM_MAX_LOCAL) snprintf(m->locals[m->nlocal++], XS_CAND_LEN, "%s", t);
    xs_unlock();
}

char *xsm_rtc_take_answer(xsm_t *m, unsigned gen) {
    char *a = NULL;
    xs_lock();
    if (gen == m->gen && m->answer) { a = m->answer; m->answer = NULL; m->answer_taken = 1; }
    xs_unlock();
    return a;
}

int xsm_rtc_take_remote(xsm_t *m, unsigned gen, char (*out)[XS_CAND_LEN], int max) {
    int k = 0;
    xs_lock();
    if (gen == m->gen && m->answer_taken) {
        while (m->remote_taken < m->nremote && k < max) {
            memcpy(out[k++], m->remotes[m->remote_taken++], XS_CAND_LEN);
        }
    }
    xs_unlock();
    return k;
}

void xsm_rtc_report(xsm_t *m, unsigned gen, int ev) {
    xs_lock();
    if (gen == m->gen) {
        if (ev == XSM_RTC_ICE_UP) m->ev_ice_up = 1;
        else if (ev == XSM_RTC_MEDIA_UP) m->ev_media_up = 1;
        else if (ev == XSM_RTC_VIDEO) m->ev_video = 1;
        else if (ev == XSM_RTC_DEAD) m->ev_dead = 1;
        else if (ev == XSM_RTC_DROPPED) m->ev_dropped = 1;
    }
    xs_unlock();
}

/* ---------------------------------------------------------------- passos */

static void add_remotes(xsm_t *m, const xs_cands_t *c) {
    if (c->end) m->remote_end = 1;
    xs_lock();
    for (int i = 0; i < c->n && m->nremote < XSM_MAX_REMOTE; i++)
        snprintf(m->remotes[m->nremote++], XS_CAND_LEN, "%s", c->c[i]);
    xs_unlock();
    for (int i = 0; i < c->n; i++) xc_log("[ice] +remoto: %.80s", c->c[i]);
}

/* Manda os locais novos da caixa. Sem nenhum ainda e `fallback`: tira as
 * linhas a=candidate: do offer (= pre-SRD do capture-rtp). */
static int send_locals(xsm_t *m, int fallback) {
    const char *list[XSM_MAX_LOCAL];
    char buf[XSM_MAX_LOCAL][XS_CAND_LEN];
    int n = 0, total;
    xs_lock();
    total = m->nlocal;
    for (int i = m->locals_sent; i < total && n < XSM_MAX_LOCAL; i++) {
        memcpy(buf[n], m->locals[i], XS_CAND_LEN);
        list[n] = buf[n];
        n++;
    }
    xs_unlock();
    int from_offer = 0;
    if (n == 0 && fallback && m->offer_sent) {
        const char *p = m->offer_sent;
        while ((p = strstr(p, "a=candidate:")) != NULL && n < XSM_MAX_LOCAL) {
            size_t l = strcspn(p + 2, "\r\n");
            if (l < XS_CAND_LEN) {
                memcpy(buf[n], p + 2, l);
                buf[n][l] = 0;
                list[n] = buf[n];
                n++;
            }
            p += 2 + l;
        }
        from_offer = 1;
    }
    if (n == 0) return 0;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    int r = xs_ice_send(&m->s, list, n, m->ufrag, &e);
    if (r == XS_OK) {
        xc_log("[ice] %d candidatos locais enviados%s", n, from_offer ? " (do offer)" : "");
        if (!from_offer) m->locals_sent = total;
    } else {
        xc_log("[ice] envio falhou: %s", e.msg);   /* capture-rtp: só loga */
    }
    return 1;
}

static void st_auth(xsm_t *m) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    ui(m, "STARTING", "Autenticando na Microsoft...");
    if (!m->has_token) {
        int r = xs_creds_load(&m->creds, &e);
        if (r != XS_OK) { fail(m, "Faca o login no Xbox primeiro"); return; }
        m->has_token = 1;
    }
    int r = xs_creds_login(&m->creds, &e);
    if (r != XS_OK) { login_fail(m, r, &e); return; }
    m->attempt = 1;
    /* Never enumerate/delete cloud sessions when connecting to a home Xbox. */
    go(m, m->cfg.home ? XSM_P_START : XSM_P_ACTIVE);
}

static void delete_active(xsm_t *m, const char *keep) {
    for (int i = 0; i < m->nactive; i++) {
        if (keep && !strcmp(m->active[i].path, keep)) continue;
        xc_err_t e;
        memset(&e, 0, sizeof(e));
        int r = xs_delete_path(&m->creds, m->active[i].path, &e);
        xc_log("[sess] sessao antiga encerrada: %s", r == XS_OK ? "ok" : e.msg);
    }
    m->nactive = 0;
}

static int find_same_title(xsm_t *m) {
    for (int i = 0; i < m->nactive; i++)
        if (m->active[i].title[0] && !strcmp(m->active[i].title, m->cfg.title_id)) return i;
    return -1;
}

static void resume_active(xsm_t *m, int idx) {
    char path[sizeof(m->active[0].path)];
    snprintf(path, sizeof(path), "%s", m->active[idx].path);
    xc_log("[sess] retomando sessao ativa (%s)", m->active[idx].state[0] ? m->active[idx].state : "?");
    delete_active(m, path);
    xs_attach(&m->s, path, m->cfg.title_id);
    m->resumed = 1;
    m->connected = 0;
    m->last_ss = XS_SS_NONE;
    m->t_state = xcn_now_ms();
    m->net_errs = 0;
    ui_phase(m, "Retomando a sessao...");
    go(m, XSM_P_POLL);
}

static void st_active(xsm_t *m) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    ui(m, "STARTING", "Verificando sessoes ativas...");
    int r = xs_active_list(&m->creds, m->active, XS_MAX_ACTIVE, &m->nactive, &e);
    if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return; }
    if (r != XS_OK) {   /* green-nx ignora: segue pra sessão nova */
        xc_log("[sess] /active: %s (ignorado)", e.msg);
        m->nactive = 0;
    }
    if (m->nactive == 0) { go(m, XSM_P_START); return; }
    xc_log("[sess] %d sessao(oes) ativa(s) na conta", m->nactive);
    int same = find_same_title(m);
    if (m->cfg.active_policy == XSM_ACTIVE_RESUME && same >= 0) { resume_active(m, same); return; }
    if (m->cfg.active_policy == XSM_ACTIVE_ASK && same >= 0) {
        xs_lock();
        m->ask_answer = -1;
        xs_unlock();
        ui(m, "STARTING", "Sessao ja ativa: A retoma, B cancela");
        go(m, XSM_P_ASK);
        return;
    }
    ui(m, "STARTING", "Encerrando sessao antiga...");
    delete_active(m, NULL);
    go(m, XSM_P_START);
}

static int st_ask(xsm_t *m) {
    xs_lock();
    int a = m->ask_answer;
    xs_unlock();
    if (a < 0) return 100;
    int same = find_same_title(m);
    if (a == 1 && same >= 0) { resume_active(m, same); return 0; }
    ui(m, "STARTING", "Encerrando sessao antiga...");
    delete_active(m, NULL);
    go(m, XSM_P_START);
    return 0;
}

static void st_start(xsm_t *m) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    m->resumed = 0;
    m->connected = 0;
    m->last_ss = XS_SS_NONE;
    m->eta_s = -1;
    ui(m, "STARTING", "Iniciando %s...", m->cfg.name);
    xc_log("[sess] ===== tentativa %d/%d: play %s =====", m->attempt, m->cfg.max_attempts, m->cfg.title_id);
    int r = m->cfg.home ? xs_home_start(&m->s, m->cfg.title_id, &e)
                        : xs_start(&m->s, m->cfg.title_id, &e);
    if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return; }
    if (r != XS_OK) { attempt_fail(m, "Falha ao iniciar %s: %s", m->cfg.home ? "Remote Play" : "jogo no xCloud", e.msg); return; }
    m->t_state = xcn_now_ms();
    m->t_next_wt = 0;
    m->net_errs = 0;
    go(m, XSM_P_POLL);
}

static int st_poll(xsm_t *m) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    xs_status_t st;
    uint64_t now;
    int r = xs_get_state(&m->s, &st, &e);
    if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return 0; }
    if (r == XS_ENOTFOUND) { attempt_fail(m, "Sessao %s sumiu", m->cfg.home ? "Remote Play" : "xCloud"); return 0; }
    if (r != XS_OK) {
        if (++m->net_errs >= 5) { attempt_fail(m, "Sem resposta do %s: %s", m->cfg.home ? "Remote Play" : "xCloud", e.msg); return 0; }
        xc_log("[sess] state falhou (%d/5): %s", m->net_errs, e.msg);
        return m->cfg.poll_ms;
    }
    m->net_errs = 0;
    now = xcn_now_ms();
    if (st.st != m->last_ss) {
        m->last_ss = st.st;
        m->t_state = now;
        xc_log("[sess] estado: %s", st.raw[0] ? st.raw : "?");
    }
    switch (st.st) {
    case XS_SS_WAITING: {
        if (m->cfg.home) { ui_phase(m, "Aguardando seu Xbox ficar disponivel..."); break; }
        if (m->cfg.waittime && now >= m->t_next_wt) {   /* v0.21.34: sempre (o do state vinha 0) */
            int sec = -1;
            xc_err_t e2;
            memset(&e2, 0, sizeof(e2));
            if (xs_wait_time(&m->s, &sec, &e2) == XS_OK) m->eta_s = sec;
            m->t_next_wt = xcn_now_ms() + 30000;
        }
        char q[160];
        int waited = (int)((now - m->t_state) / 1000);
        int eta = m->eta_s >= 60 ? m->eta_s : st.wait_s;   /* o /v1/waittime primeiro */
        queue_text(q, sizeof(q), st.queue_pos, eta, waited);
        ui(m, "QUEUE", "%s", q);
        break;
    }
    case XS_SS_READY:
        /* Home sessions need no cloud transfer/passport token. Wait for
         * Provisioned before exchanging SDP (same transport afterwards). */
        if (m->cfg.home) { ui_phase(m, "Preparando seu Xbox..."); break; }
        ui_phase(m, "Preparando servidor...");
        if (!m->connected) {
            xc_log("[sess] ReadyToConnect: /connect (passport)");
            r = xs_connect(&m->s, &e);
            if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return 0; }
            if (r != XS_OK) { attempt_fail(m, "Falha ao autenticar a sessao: %s", e.msg); return 0; }
            m->connected = 1;
        }
        break;
    case XS_SS_PROVISIONED:
        ui_phase(m, "Conectando video...");
        new_gen(m);
        xs_lock();
        m->want_offer = 1;
        xs_unlock();
        m->t_vid0 = xcn_now_ms();
        go(m, XSM_P_WAIT_OFFER);
        return 0;
    case XS_SS_FAILED:
        if (st.err_code[0] || st.err_msg[0])
            attempt_fail(m, "Sessao %s falhou %s", m->cfg.home ? "Remote Play" : "xCloud", st.err_code[0] ? st.err_code : st.err_msg);
        else
            attempt_fail(m, "Sessao %s falhou", m->cfg.home ? "Remote Play" : "xCloud");
        return 0;
    default:
        ui_phase(m, "Preparando servidor...");
        break;
    }
    int cap = m->resumed ? m->cfg.resume_timeout_s
            : st.st == XS_SS_WAITING && !m->cfg.home ? m->cfg.queue_timeout_s : m->cfg.state_timeout_s;
    if (xcn_now_ms() - m->t_state >= (uint64_t)cap * 1000) {
        attempt_fail(m, "Tempo esgotado esperando o %s (%s)", m->cfg.home ? "Remote Play" : "xCloud", st.raw[0] ? st.raw : "?");
        return 0;
    }
    return m->cfg.poll_ms;
}

static int st_wait_offer(xsm_t *m) {
    char *offer = NULL;
    xs_lock();
    if (m->offer) { offer = m->offer; m->offer = NULL; m->want_offer = 0; }
    xs_unlock();
    if (!offer) {
        if (xcn_now_ms() - m->t_phase >= (uint64_t)m->cfg.offer_timeout_s * 1000)
            attempt_fail(m, "WebRTC nao gerou a oferta");
        return 50;
    }
    free(m->offer_sent);
    m->offer_sent = offer;
    xs_sdp_ufrag(offer, m->ufrag, sizeof(m->ufrag));
    xc_log("[sdp] offer %u bytes, ice-ufrag local %.4s...", (unsigned)strlen(offer), m->ufrag);
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    int r = xs_sdp_offer(&m->s, offer, &e);
    if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return 0; }
    if (r != XS_OK) { attempt_fail(m, "Falha na negociacao SDP: %s", e.msg); return 0; }
    go(m, XSM_P_SDP);
    return m->cfg.sdp_poll_ms;
}

static int st_sdp(xsm_t *m) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    char *ans = NULL;
    int r = xs_sdp_poll(&m->s, &ans, &e);
    if (r == XS_PENDING) {
        if (xcn_now_ms() - m->t_phase >= (uint64_t)m->cfg.sdp_timeout_s * 1000)
            attempt_fail(m, "Falha na negociacao SDP: sem answer");
        return m->cfg.sdp_poll_ms;
    }
    if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return 0; }
    if (r != XS_OK) { attempt_fail(m, "Falha na negociacao SDP: %s", e.msg); return 0; }
    char ru[64];
    xs_sdp_ufrag(ans, ru, sizeof(ru));
    xc_log("[sdp] answer %u bytes, ice-ufrag remoto %.4s...", (unsigned)strlen(ans), ru);
    free(m->answer_tmp);
    m->answer_tmp = ans;
    go(m, XSM_P_ICE_BRIDGE);
    send_locals(m, 1);   /* pre-SRD: locais primeiro */
    return 0;
}

static void publish_answer(xsm_t *m) {
    xs_lock();
    free(m->answer);
    m->answer = m->answer_tmp;
    m->answer_tmp = NULL;
    int nr = m->nremote;
    xs_unlock();
    xc_log("[ice] pre-SRD pronto: %d remotos; answer publicado", nr);
    uint64_t now = xcn_now_ms();
    m->t_media = now;
    m->t_next_ka = now + (uint64_t)m->cfg.keepalive_ms;
    go(m, XSM_P_CONNECTING);
}

static int st_ice_bridge(xsm_t *m) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    xs_cands_t c;
    int r = xs_ice_poll(&m->s, &c, &e);
    if (r == XS_ELOGIN || r == XS_EAUTH) { login_fail(m, r, &e); return 0; }
    if (r == XS_OK) add_remotes(m, &c);
    else if (r != XS_PENDING) xc_log("[ice] pre-SRD poll: %s", e.msg);
    int have;
    xs_lock();
    have = m->nremote;
    xs_unlock();
    if (have > 0 || xcn_now_ms() - m->t_phase >= (uint64_t)m->cfg.pre_srd_ms) {
        publish_answer(m);
        return 0;
    }
    return m->cfg.ice_poll_ms;
}

static void resignal(xsm_t *m) {
    uint64_t now = xcn_now_ms();
    if (!m->resignal_window || now - m->resignal_window > 120000) {
        m->resignal_window = now;
        m->resignals = 0;
    }
    if (++m->resignals > m->cfg.max_resignals) {
        fail(m, "A conexao caiu varias vezes, comece de novo");
        return;
    }
    xc_log("[sess] queda no meio do jogo (%d/%d em 2 min): re-sinalizando a mesma sessao",
           m->resignals, m->cfg.max_resignals);
    new_gen(m);
    m->resumed = 1;
    m->connected = 0;
    m->last_ss = XS_SS_NONE;
    m->t_state = now;
    m->net_errs = 0;
    ui_phase(m, "Preparando servidor...");
    go(m, XSM_P_POLL);
}

/* CONNECTING / STREAM_WAIT / STREAMING: eventos do WebRTC e prazos.
 * net = 0 (thread da sessão): consome dead/dropped e faz as transições de
 *   falha (attempt_fail = DELETE..., resignal) — exatamente como antes.
 * net = 1 (thread de rede, v0.19.2): só as transições baratas (ICE up,
 *   mídia -> STREAM_WAIT, vídeo -> STREAMING); qualquer falha devolve 1 sem
 *   consumir nada, e a thread da sessão refaz a mesma decisão.
 * 0 = segue pros pedidos; 1 = (rede) precisa da thread; 2 = (thread) a
 * fase mudou por falha/re-sinalização. */
static int live_events(xsm_t *m, int net) {
    int ice_up, media_up, video, dead, dropped;
    xs_lock();
    ice_up = m->ev_ice_up;
    media_up = m->ev_media_up;
    video = m->ev_video;
    dead = m->ev_dead;
    dropped = m->ev_dropped;
    if (!net) m->ev_dead = m->ev_dropped = 0;
    xs_unlock();
    uint64_t now = xcn_now_ms();

    if (m->phase == XSM_P_CONNECTING) {
        if (ice_up && !m->t_ice_up) { m->t_ice_up = now; xc_log("[sess] ICE conectou: janela de %ds pro DTLS", m->cfg.dtls_wait_s); }
        if (media_up) {
            ui_phase(m, "Aguardando video...");
            go(m, XSM_P_STREAM_WAIT);
        } else if (dead || dropped ||
                   (m->t_ice_up && now - m->t_ice_up > (uint64_t)m->cfg.dtls_wait_s * 1000) ||
                   now - m->t_media > (uint64_t)m->cfg.media_deadline_s * 1000) {
            if (net) return 1;
            attempt_fail(m, "xCloud nao conectou (midia morta)");
            return 2;
        }
    }
    if (m->phase == XSM_P_STREAM_WAIT) {
        if (video) {
            m->attempt = 1;        /* budget cheio pra um fallback no meio do jogo */
            m->resumed = 0;
            ui(m, "STREAMING", "%s", m->cfg.name);
            go(m, XSM_P_STREAMING);
        } else if (dead || dropped) {
            if (net) return 1;
            if (m->resignals > 0) { resignal(m); return 2; }
            attempt_fail(m, "xCloud nao conectou (midia morta)");
            return 2;
        } else if (now > m->t_phase && now - m->t_phase > (uint64_t)m->cfg.first_video_s * 1000) {
            /* stage 5 fix: `now` is read at the top of st_live, BEFORE go()
             * stamps t_phase in the CONNECTING branch above; on a real clock
             * t_phase can be 1 ms ahead and the unsigned difference wrapped,
             * failing every attempt the instant DTLS came up (the host mock's
             * virtual clock never advances inside a step, so the unit tests
             * could not see it). */
            if (net) return 1;
            attempt_fail(m, "O video do xCloud nao chegou");
            return 2;
        }
    }
    if (m->phase == XSM_P_STREAMING && (dead || dropped)) {
        if (net) return 1;
        resignal(m);
        return 2;
    }
    return 0;
}

static int st_live(xsm_t *m) {
    if (live_events(m, 0)) return 0;
    uint64_t now = xcn_now_ms();

    /* um pedido por passo: keepalive > locais novos > remotos */
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (now >= m->t_next_ka) {
        m->t_next_ka = now + (uint64_t)m->cfg.keepalive_ms;
        int r = xs_keepalive(&m->s, &e);
        if (r == XS_ENOTFOUND) {
            attempt_fail(m, "Sessao xCloud encerrada pelo servidor");
            return 0;
        }
        if (r != XS_OK) xc_log("[sess] keepalive falhou: %s", e.msg);
        return 0;
    }
    if (send_locals(m, 0)) return 0;
    /* stage 5: no ICE polling once the video flows. Every poll is a fresh TLS
     * handshake (~75 ms CPU on the session hw thread, the decoder's SMT
     * sibling); if xCloud never sends end-of-candidates, 200 ms polling
     * would cost ~1/3 of that thread for the whole game. Re-signaling starts
     * a new generation (remote_end = 0, phase POLL) and polls again. */
    if (!m->remote_end && m->phase != XSM_P_STREAMING) {
        xs_cands_t c;
        int r = xs_ice_poll(&m->s, &c, &e);
        if (r == XS_OK) add_remotes(m, &c);
        else if (r != XS_PENDING) xc_log("[ice] poll: %s", e.msg);
        return m->cfg.ice_poll_ms;
    }
    uint64_t wait = m->t_next_ka > now ? m->t_next_ka - now : 0;
    return (int)(wait < 100 ? wait : 100);
}

int xsm_step(xsm_t *m) {
    xs_lock();
    int stop = m->stop_req;
    xs_unlock();
    if (stop && m->phase != XSM_P_DONE && m->phase != XSM_P_ERROR && m->phase != XSM_P_STOPPING)
        go(m, XSM_P_STOPPING);

    /* v0.21.10: offer + SDP + ICE com prazo TOTAL (cada fase tinha o seu, e
     * somados davam minutos em "Conectando video" parecendo travado) */
    if (m->t_vid0 && (m->phase == XSM_P_WAIT_OFFER || m->phase == XSM_P_SDP || m->phase == XSM_P_ICE_BRIDGE)) {
        uint64_t el = xcn_now_ms() - m->t_vid0;
        if (m->cfg.video_total_s > 0 && el >= (uint64_t)m->cfg.video_total_s * 1000) {
            const char *ph = xsm_phase_name(m->phase);
            m->t_vid0 = 0;
            attempt_fail(m, "Tempo esgotado conectando o video (%s, %us)", ph, (unsigned)(el / 1000));
            return 0;
        }
    } else if (m->phase != XSM_P_WAIT_OFFER && m->phase != XSM_P_SDP && m->phase != XSM_P_ICE_BRIDGE) {
        m->t_vid0 = 0;
    }
    switch (m->phase) {
    case XSM_P_IDLE:        go(m, XSM_P_AUTH); return 0;
    case XSM_P_AUTH:        st_auth(m); return 0;
    case XSM_P_ACTIVE:      st_active(m); return 0;
    case XSM_P_ASK:         return st_ask(m);
    case XSM_P_START:       st_start(m); return 0;
    case XSM_P_POLL:        return st_poll(m);
    case XSM_P_WAIT_OFFER:  return st_wait_offer(m);
    case XSM_P_SDP:         return st_sdp(m);
    case XSM_P_ICE_BRIDGE:  return st_ice_bridge(m);
    case XSM_P_CONNECTING:
    case XSM_P_STREAM_WAIT:
    case XSM_P_STREAMING:   return st_live(m);
    case XSM_P_COOLDOWN: {
        uint64_t el = xcn_now_ms() - m->t_phase;
        if (el < (uint64_t)m->cfg.cooldown_ms) return (int)((uint64_t)m->cfg.cooldown_ms - el);
        m->attempt++;
        go(m, XSM_P_START);
        return 0;
    }
    case XSM_P_STOPPING:
        xs_stop(&m->s);
        new_gen(m);
        ui(m, "IDLE", "Sessao encerrada");
        go(m, XSM_P_DONE);
        return -1;
    case XSM_P_DONE:
    case XSM_P_ERROR:
    default:
        return -1;
    }
}

/* ================================================= v0.19.2: dono / rede */

int xsm_live_phase(xsm_phase_t p) {
    return p == XSM_P_CONNECTING || p == XSM_P_STREAM_WAIT || p == XSM_P_STREAMING;
}

int xsm_owner(xsm_t *m) { xs_lock(); int o = m->owner; xs_unlock(); return o; }
void xsm_set_owner(xsm_t *m, int owner) { xs_lock(); m->owner = owner; xs_unlock(); }

int xsm_try_handoff(xsm_t *m, uint64_t t0) {
    if (!m->cfg.net_live || !xsm_live_phase(m->phase)) return 0;
    /* fase ao vivo: a thread de rede assume (só a cifra dos pedidos
     * keep-alive) e a hw 3 fica pro decoder. Nada de tocar na máquina
     * depois do owner = NET. */
    int handed = 0;
    const char *ph = xsm_phase_name(m->phase);
    unsigned nh = m->handoffs + 1;
    xs_lock();
    if (!m->stop_req && !m->ev_dead && !m->ev_dropped) { m->handoffs = nh; m->owner = XSM_OWN_NET; handed = 1; }
    xs_unlock();
    if (handed)
        xc_log("[sess] fase %s: sessao entregue a thread de rede (hw1, #%u), thread da sessao sai em %ums", ph, nh,
               (unsigned)(xcn_now_ms() - t0));
    return handed;
}

int xsm_thread_run(xsm_t *m) {
    uint64_t t0 = xcn_now_ms();
    for (;;) {
        int d = xsm_step(m);
        if (d < 0) break;
        if (xsm_try_handoff(m, t0)) return 0;
        /* espera em fatias de 50ms pra reagir rápido a stop/eventos do WebRTC */
        uint64_t end = xcn_now_ms() + (uint64_t)d;
        while (xcn_now_ms() < end) {
            xs_lock();
            int stop = m->stop_req;
            xsm_phase_t ph = m->phase;   /* só esta thread escreve */
            int ev = ((m->ev_dead | m->ev_dropped) && ph >= XSM_P_CONNECTING && ph <= XSM_P_STREAMING) ||
                     (m->offer != NULL && ph == XSM_P_WAIT_OFFER) ||
                     (m->ev_media_up && ph == XSM_P_CONNECTING) ||
                     (m->ev_video && ph == XSM_P_STREAM_WAIT);
            xs_unlock();
            if (stop || ev) break;
            int left = (int)(end - xcn_now_ms());
            xcn_sleep_ms(left < 50 ? (left > 0 ? left : 1) : 50);
        }
    }
    xc_log("[sess] thread da sessao terminou (%s) em %us", xsm_phase_name(xsm_phase(m)),
           (unsigned)((xcn_now_ms() - t0) / 1000));
    xs_creds_free(&m->creds);   /* tokens fora da RAM já (fecha a conexão keep-alive) */
    xsm_set_owner(m, XSM_OWN_DONE);
    return 1;
}

static int net_escalate(xsm_t *m, const char *why) {
    snprintf(m->esc_why, sizeof(m->esc_why), "%s", why);
    m->escalations++;
    xc_log("[sess] hw1 devolve a sessao pra thread (%s, fase %s, #%u)", why, xsm_phase_name(m->phase),
           m->escalations);
    xsm_set_owner(m, XSM_OWN_WANT_THREAD);
    return 1;
}

int xsm_net_step(xsm_t *m) {
    uint64_t now = xcn_now_ms();
    xs_lock();
    int stop = m->stop_req;
    xs_unlock();
    xc_err_t e;
    memset(&e, 0, sizeof(e));

    if (m->s.aq) {
        int kind = m->s.aq;
        xs_cands_t c;
        int r = xs_async_poll(&m->s, &c, &e);
        if (r == XS_INPROGRESS) {
            /* STOP com pedido em voo: espera ele (a conexão serve pro DELETE) */
            if (stop && now - m->t_aq > 3000) { xs_async_abort(&m->s); return net_escalate(m, "stop"); }
            return 0;
        }
        m->t_net_next = now + (uint64_t)m->aq_then_ms;
        if (kind == XS_AQ_KEEPALIVE) {
            if (r == XS_ENEEDCPU || r == XS_ENOTFOUND) {
                m->t_next_ka = 0;   /* a thread refaz este keepalive já (re-login / 404 / reconexão) */
                return net_escalate(m, r == XS_ENOTFOUND ? "keepalive 404" : e.msg[0] ? e.msg : "keepalive");
            }
            if (r != XS_OK) xc_log("[sess] keepalive falhou: %s", e.msg);
        } else if (kind == XS_AQ_ICE_SEND) {
            if (r == XS_ENEEDCPU) return net_escalate(m, e.msg[0] ? e.msg : "ice");
            if (r == XS_OK) {
                xc_log("[ice] %d candidatos locais enviados", m->aq_total - m->locals_sent);
                m->locals_sent = m->aq_total;
            } else {
                xc_log("[ice] envio falhou: %s", e.msg);
                m->t_net_next = now + (uint64_t)m->cfg.ice_poll_ms;
            }
        } else if (kind == XS_AQ_ICE_POLL) {
            if (r == XS_ENEEDCPU) return net_escalate(m, e.msg[0] ? e.msg : "ice poll");
            if (r == XS_OK) add_remotes(m, &c);
            else if (r != XS_PENDING) xc_log("[ice] poll: %s", e.msg);
        }
        memset(&e, 0, sizeof(e));
    }
    if (stop) return net_escalate(m, "stop");
    if (!xsm_live_phase(m->phase)) return net_escalate(m, "fase");
    if (live_events(m, 1)) return net_escalate(m, "evento do webrtc/prazo");
    if (now < m->t_net_next) return 0;

    /* um pedido por vez: keepalive > locais novos > remotos (= st_live) */
    if (now >= m->t_next_ka) {
        m->t_next_ka = now + (uint64_t)m->cfg.keepalive_ms;
        int r = xs_async_start(&m->s, XS_AQ_KEEPALIVE, NULL, 0, NULL, &e);
        if (r == XS_ENEEDCPU) { m->t_next_ka = 0; return net_escalate(m, e.msg[0] ? e.msg : "keepalive"); }
        if (r != XS_OK) { xc_log("[sess] keepalive falhou: %s", e.msg); return 0; }
        m->t_aq = now;
        m->aq_then_ms = 0;
        return 0;
    }
    {
        const char *list[XSM_MAX_LOCAL];
        char buf[XSM_MAX_LOCAL][XS_CAND_LEN];
        int n = 0, total;
        xs_lock();
        total = m->nlocal;
        for (int i = m->locals_sent; i < total && n < XSM_MAX_LOCAL; i++) {
            memcpy(buf[n], m->locals[i], XS_CAND_LEN);
            list[n] = buf[n];
            n++;
        }
        xs_unlock();
        if (n > 0) {
            int r = xs_async_start(&m->s, XS_AQ_ICE_SEND, list, n, m->ufrag, &e);
            if (r == XS_ENEEDCPU) return net_escalate(m, e.msg[0] ? e.msg : "ice");
            if (r != XS_OK) { xc_log("[ice] envio falhou: %s", e.msg); m->t_net_next = now + (uint64_t)m->cfg.ice_poll_ms; return 0; }
            m->aq_total = total;
            m->t_aq = now;
            m->aq_then_ms = 0;
            return 0;
        }
    }
    if (!m->remote_end && m->phase != XSM_P_STREAMING) {
        int r = xs_async_start(&m->s, XS_AQ_ICE_POLL, NULL, 0, NULL, &e);
        if (r == XS_ENEEDCPU) return net_escalate(m, e.msg[0] ? e.msg : "ice poll");
        if (r != XS_OK) { xc_log("[ice] poll: %s", e.msg); m->t_net_next = now + (uint64_t)m->cfg.ice_poll_ms; return 0; }
        m->t_aq = now;
        m->aq_then_ms = m->cfg.ice_poll_ms;
        return 0;
    }
    m->t_net_next = now + 20;
    return 0;
}
