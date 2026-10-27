/*
 * XCloud360 stage 5 — the Xbox plays xCloud with NO PC.
 *
 * Glue between the menu (main, hw0), the session thread (stage 3,
 * source/session: login refresh, /play, queue, /sdp, /ice, keepalive,
 * DELETE — HTTPS, hw XST_HW_THREAD) and the WebRTC receiver (stage 4,
 * source/webrtc: ICE/DTLS/SRTP/SCTP, jitter + NACK, datachannels — ALL on
 * the hw1 network thread, next to lwIP). Portable C: the same file runs in
 * the offline host E2E (hosttest/standalone) with pthreads and POSIX UDP.
 *
 * Threads:
 *   main  xn_init (boot, before the network thread), xn_play / xn_stop /
 *         xn_state_line / xn_request_keyframe / xn_stat_tail (menu + game).
 *   hw1   xn_net_step() every net_step (~1 ms) and xn_pad_frame() from the
 *         input poll: attach/detach, xcws_step (= xcw_poll), callbacks
 *         (video AU -> platform inject, Opus -> platform audio), stats.
 *   sess  xs_task (xst_*): per PLAY, from AUTH until the answer is
 *         published (play, queue, /connect, SDP, ICE bridge). v0.19.2: then
 *         it hands the live phases (CONNECTING/STREAM_WAIT/STREAMING) to hw1
 *         and EXITS, so the decoder gets hw3 back (4 threads, hw 4,2,5,3).
 *         hw1 drives keepalive/ICE/events non-blocking over the session's
 *         keep-alive HTTPS connection (xsm_net_step). Anything that needs
 *         real CPU or waiting (401 re-login, full TLS handshake, re-signal,
 *         attempt failure, the final DELETE) gives the machine back and
 *         hw1 re-spawns the session thread on hw3 as soon as hw3 is free:
 *         at stop the decoder has closed; mid-game main lends hw3 by
 *         reopening the decoder with 3 threads (xn_dec_threads_wanted) and
 *         takes it back (4) once the session is on hw1 again.
 *
 * Lifetime rule (the reason this file exists): xst_start re-initialises the
 * session machine that hw1 reads through the xsm_rtc_* mailbox, so a new
 * PLAY is accepted only when hw1 has DETACHED from the previous one (PC
 * destroyed, acknowledged) and the previous session thread has exited (its
 * DELETE went out). xn_can_play() says when.
 */
#ifndef XC360_XC_NATIVE_H
#define XC360_XC_NATIVE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcw xcw_t;

typedef struct {
    /* hw1: [AU] into the decoder ring. 0 = queued, -1 = no room (the AU is
     * dropped and the receiver waits for the next IDR, keyframe requested).
     * idr / params = IDR slice inside / SPS+PPS prepended from the cache. */
    int  (*inject_au)(const uint8_t *au, size_t n, int idr, int params);
    /* hw1: one Opus packet, same content as the :9004 path. */
    void (*audio)(uint16_t seq, uint32_t rtp_ts, const uint8_t *opus, size_t n);
    /* any thread: one diagnostic line (goes to the PC LOG listener). Never
     * receives tokens. */
    void (*log)(const char *line);
    /* main, right before the session thread starts (login cancel flag). */
    void (*before_session)(void);
    /* hw1, optional: every datachannel message (ch = xcw_channel_t). */
    void (*channel_msg)(int ch, const uint8_t *data, size_t n);
} xn_platform_t;

typedef struct {
    int video_kbps;        /* b=AS/TIAS + REMB + maxBitrate (launcher: 1500) */
    int keepalive_ms;      /* session /keepalive period (capture-rtp 5000) */
    int consent_ms;        /* our ICE consent checks, 0 = off (capture-rtp) */
    int max_attempts;      /* new xCloud sessions per PLAY (capture-rtp 2) */
} xn_cfg_t;

enum { XN_OK = 0, XN_EBUSY = -1, XN_ESTART = -2, XN_EINIT = -3 };

/* main, once, before the network thread runs xn_net_step. */
int  xn_init(const xn_platform_t *p, const xn_cfg_t *cfg);
/* main: start a native session (session thread + hw1 attach). */
int  xn_play(const char *title_id, const char *name, int f2p);
/* main: B / Back+Start / error: DELETE the session and detach WebRTC. */
void xn_stop(void);
/* main: 1 when a new xn_play would be accepted. */
int  xn_can_play(void);
/* main: 1 between xn_play and the end of teardown (session or hw1 busy). */
int  xn_busy(void);
/* main: "STATE\tCODE\ttext" for mm_handle_line(); returns a seq that
 * changes with every update (0 = never started). */
unsigned xn_state_line(char *out, size_t cap);
/* main: the decoder wants an IDR (KFRQ of the PC path). force = ignore the
 * 1/s throttle (fresh consumer). */
void xn_request_keyframe(int force);
/* main: " rtc=... rtt_ms=... loss=... kbps=... nack=... kfrq=... srtp_err=...
 * hw1=..." appended to the 2 s STAT line. Returns the length. */
int  xn_stat_tail(char *dst, size_t n);
/* main: one-shot summary for the menu after a session. */
void xn_summary(char *dst, size_t n);

/* ---- v0.19.3: Back+Start volta pro menu com a sessão ABERTA ----
 * xn_pause (main, saindo do jogo): a sessão segue (keepalive na hw1, ICE,
 * RTCP), os controles vão a neutro no xCloud e o PC passa a jogar fora o
 * vídeo (antes do on_video_au: nada chega no decoder/ring) e o áudio, sem
 * pedir keyframe nenhum. Uma falha da tentativa enquanto pausado vira
 * ERROR (xsm_set_keep: sem sessão nova sozinha).
 * xn_resume (main, o MESMO título escolhido de novo): volta na hora, sem
 * fila nem /play: IDR gate ligado + keyframe forçado (PLI +
 * videoKeyframeRequested); o 1o AU que chega no inject_au é um IDR.
 * Outro título: xn_stop (DELETE) e xn_play quando xn_can_play() — a mesma
 * regra de sempre.
 * xn_session (main): o que há agora; com a sessão pausada e a máquina já
 * terminada (o xCloud derrubou: keepalive 404, erro...) ela mesma faz o
 * xn_stop (solta o WebRTC), então chamar no loop do menu deixa tudo limpo. */
enum { XN_SESS_NONE = 0,  /* nada: xn_play aceita (ver xn_can_play) */
       XN_SESS_STARTING,  /* xn_play feito, sessão subindo/re-sinalizando */
       XN_SESS_PLAYING,   /* no jogo (STREAMING) */
       XN_SESS_PAUSED,    /* aberta, no menu (xn_pause) */
       XN_SESS_ENDING };  /* xn_stop feito, DELETE/teardown em curso */
/* XN_OK, ou XN_EBUSY (nada vivo pra pausar: chame xn_stop). */
int  xn_pause(void);
/* XN_OK, ou XN_EBUSY (não está pausada, já acabou ou é outro título). */
int  xn_resume(const char *title_id);
/* XN_SESS_*; copia o título/nome do último xn_play (vazio com NONE). */
int  xn_session(char *title_id, size_t tn, char *name, size_t nn);

/* hw1 (network thread) */
void xn_net_step(void);
/* hw1: one 24-byte pad frame (main.c send_pad wire format, buf[0] = xCloud
 * player index). Ignored unless a session is attached. */
void xn_pad_frame(const uint8_t *buf, size_t n);
/* hw1: neutral frames for every pad that was used (leaving the game). */
void xn_pad_neutral(void);
/* hw1: the live PeerConnection or NULL (tests: chat ping). */
xcw_t *xn_pc(void);
/* hw1 load measured by the platform (busy us / wall us since the last
 * call), reported in the STAT tail. Optional. */
void xn_set_load_probe(void (*fn)(uint64_t *busy_us, uint64_t *wall_us));

/* ---- v0.19.2: the session's hw thread (XST_HW_THREAD = 3) is leased
 * either to the decoder (its 4th frame thread) or to the session thread,
 * never both (libxenon runs one task per hw thread; pthread_create would
 * fail and xenon_run_thread_task is not atomic). Main only. ---- */
/* Before avcodec_open2 with 4 threads: 1 = hw3 is the decoder's now. */
int  xn_dec_claim_hw3(void);
/* After avcodec_close (threads joined) of a decoder that held it. */
void xn_dec_release_hw3(void);
/* In a native game, every ~10 ms: how many frame threads the decoder
 * should have now (`cur` = current, `max` = with hw3). Returns `cur` when
 * nothing changes; max-1 = lend hw3 to the session (it needs CPU); max =
 * the session is back on hw1, take hw3 again (claim first). */
int  xn_dec_threads_wanted(int cur, int max);
/* v0.21.25: "hw3=decoder sessao=hw1" pro log do decoder */
void xn_hw3_diag(char *o, size_t n);
/* For the STAT line (dec_thr=). */
void xn_set_dec_threads(int n);
/* 1 while the session thread runs (game entry waits a little for it to
 * hand the live phases to hw1, so the decoder opens with 4). */
int  xn_session_thread_busy(void);
/* v0.21.10: ms com o vídeo perdido no jogo (WebRTC morto ou sessão esperando a hw3 do decoder); 0 = ok */
int  xn_video_lost_ms(void);

#ifdef __cplusplus
}
#endif
#endif
