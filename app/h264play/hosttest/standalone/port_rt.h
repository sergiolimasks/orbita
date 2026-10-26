/* XCloud360 stage 5 — host: see port_rt.c. */
#ifndef XC360_PORT_RT_H
#define XC360_PORT_RT_H
#include <pthread.h>
#include <stdint.h>

#define GSSV_MAX_SESS 8

typedef struct {
    char path[128];
    char dir[400];          /* pion run directory of this xCloud session */
    char title[48];
    int  state_polls, offer_written, ice_delivered;
    int  n_sdp, n_ice_post, n_ice_get, n_connect, n_ka, n_delete;
} gssv_sess_t;

typedef struct {
    pthread_mutex_t mu;
    char next_dir[400];     /* dir for the NEXT /play (set by the test) */
    int  extra_queue_polls; /* longer queue (B-cancel scenario) */
    gssv_sess_t s[GSSV_MAX_SESS];
    int  nsess, nreq;
    int  n_refresh, n_login, n_passport, n_active, n_play;
    /* v0.19.2: the fake GSSV is a real local TLS 1.2 server (mbedTLS,
     * RSA-2048 test CA made by run_standalone.sh), so the app's https.c
     * keep-alive / resumption / non-blocking paths run for real. */
    int  gs_valid;          /* the only accepted "Bearer GS-<n>" (n_login) */
    int  revoke_next_ka;    /* next /keepalive: token expired -> 401 */
    int  close_next_region; /* next region response: Connection: close */
    int  resume_ok;         /* server session cache answers (abbreviated hs) */
    int  n_401;
    /* TLS counters (by SNI) */
    int  region_conns, region_full, region_resumed;
    int  other_full, other_resumed;
    int  region_reqs, region_max_reqs_per_conn;
    int  region_ka_on_reused;   /* /keepalive answered on a conn that served >= 1 before */
    /* v0.19.3: sessão aberta no menu */
    int  ka_gone;           /* i+1: /keepalive da sessão i = 404 (o xCloud encerrou) */
    int  nev;               /* ordem dos /play ('P') e DELETE ('D') */
    char ev_kind[64];
    int  ev_sess[64];
} gssv_t;

extern gssv_t g_gssv;

void        rt_log_line(const char *line);
const char *rt_log_text(void);
const char *rt_token(void);
/* Starts the local TLS server (cert/key PEM files). 0 = ok. */
int         rt_tls_server_start(const char *cert_pem, const char *key_pem);

#endif
