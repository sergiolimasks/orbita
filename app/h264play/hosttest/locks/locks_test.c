/* orbita v0.21.14 — locks do app em duas threads (main e hw1 de mentira),
 * no ritmo do jogo, com o log beta de verdade (xc_logup.c). ThreadSanitizer.
 *   1) a ordem global do app (sessão > net_pipe > wifi status > folhas) em
 *      300 mil voltas por thread: termina, sem ordem invertida;
 *   2) o detector acha um ABBA (feito em sequência, sem travar);
 *   3) o relatório da espera diz o lock, o dono e o lugar. */
#include "xc_lock.h"
#include "xc_logup.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern __thread int xlk_test_pir;
static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static xlk_t L_sess = XLK_INIT("sessão (xs_lock)");
static xlk_t L_nprx = XLK_INIT("net_pipe rx");
static xlk_t L_nptx = XLK_INIT("net_pipe tx");
static xlk_t L_stat = XLK_INIT("wifi status");
static xlk_t L_wlog = XLK_INIT("WIFILOG");
static xlk_t L_uart = XLK_INIT("UART hw1");
static volatile long g_shared[4];

static unsigned rnd_r(unsigned *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

/* folha: só tenta (como o xc_log/xw_logfile_line) */
static void leaf(xlk_t *l, const char *site) {
    if (xlk_try(l, site, 2000)) { __sync_fetch_and_add(&g_shared[3], 1); xlk_unlock(l); }
}

static void *main_like(void *u) {
    (void)u;
    xlk_test_pir = 0;
    unsigned s = 1;
    char line[96];
    for (int i = 0; i < 300000; i++) {
        switch (rnd_r(&s) % 5) {
        case 0:   /* native_poll_state: sessão, loga dentro */
            xlk_lock(&L_sess, "main xn_session");
            g_shared[0]++;
            leaf(&L_wlog, "main log na sessão");
            xlk_unlock(&L_sess);
            break;
        case 1:   /* xw_sys_status */
            xlk_lock(&L_stat, "main xw_sys_status");
            g_shared[1]++;
            xlk_unlock(&L_stat);
            break;
        case 2:   /* STAT: linha no log beta + WIFILOG */
            snprintf(line, sizeof(line), "STAT t=%d dec_fps=58.9", i);
            xlu_line(line);
            leaf(&L_wlog, "main np_log_line");
            break;
        case 3:   /* xcn_recv (hw3/main): net_pipe rx então tx */
            xlk_lock(&L_nprx, "main xcn_recv");
            xlk_unlock(&L_nprx);
            xlk_lock(&L_nptx, "main xcn_send");
            xlk_unlock(&L_nptx);
            break;
        default:  /* drenagem da UART no menu */
            xlk_lock(&L_uart, "main uart drain");
            xlk_unlock(&L_uart);
        }
    }
    return NULL;
}

static void *hw1_like(void *u) {
    (void)u;
    xlk_test_pir = 1;
    unsigned s = 7;
    char line[96];
    for (int i = 0; i < 300000; i++) {
        switch (rnd_r(&s) % 5) {
        case 0:   /* session_drive: sessão -> log ([xn]) */
            xlk_lock(&L_sess, "hw1 session_drive");
            g_shared[0]++;
            leaf(&L_uart, "hw1 xc_log");
            leaf(&L_wlog, "hw1 xw_logfile_line");
            xlk_unlock(&L_sess);
            break;
        case 1:   /* RX + lwIP: net_pipe on_recv */
            xlk_lock(&L_nprx, "hw1 on_recv");
            g_shared[2]++;
            xlk_unlock(&L_nprx);
            break;
        case 2:   /* publish do Wi-Fi */
            xlk_lock(&L_stat, "hw1 publish");
            g_shared[1]++;
            xlk_unlock(&L_stat);
            break;
        case 3:   /* [dc]/[xn] no RX: xc_log da hw1 */
            snprintf(line, sizeof(line), "[dc] recv %d showtitledefault", i);
            leaf(&L_uart, "hw1 xc_log");
            xlu_line(line);
            leaf(&L_wlog, "hw1 xw_logfile_line");
            break;
        default:  /* np_step: tx */
            xlk_lock(&L_nptx, "hw1 np_step");
            xlk_unlock(&L_nptx);
        }
    }
    return NULL;
}

/* 2) ABBA em sequência */
static xlk_t A = XLK_INIT("A"), B = XLK_INIT("B");
static void *ab(void *u) { (void)u; xlk_test_pir = 2; xlk_lock(&A, "ab A"); xlk_lock(&B, "ab B"); xlk_unlock(&B); xlk_unlock(&A); return NULL; }
static void *ba(void *u) { (void)u; xlk_test_pir = 3; xlk_lock(&B, "ba B"); xlk_lock(&A, "ba A"); xlk_unlock(&A); xlk_unlock(&B); return NULL; }

/* 3) espera */
static xlk_t W = XLK_INIT("lento");
static volatile int g_hold = 0;
static void *holder(void *u) {
    (void)u;
    xlk_test_pir = 4;
    xlk_lock(&W, "holder pegou");
    __atomic_store_n(&g_hold, 1, __ATOMIC_SEQ_CST);
    usleep(400000);
    xlk_unlock(&W);
    return NULL;
}
static void *waiter(void *u) {
    (void)u;
    xlk_test_pir = 5;
    while (!__atomic_load_n(&g_hold, __ATOMIC_SEQ_CST)) usleep(1000);
    xlk_lock(&W, "waiter pediu");
    xlk_unlock(&W);
    return NULL;
}

int main(void) {
    xlu_init("locks-test");
    printf("--- 1) main e hw1 no ritmo do jogo (300 mil voltas cada)\n");
    pthread_t t1, t2;
    pthread_create(&t1, NULL, main_like, NULL);
    pthread_create(&t2, NULL, hw1_like, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("  sessão %ld, status %ld, rx %ld, folhas %ld; desistências %u, log beta descartado %u\n", g_shared[0], g_shared[1],
           g_shared[2], g_shared[3], xlk_try_fails(), xlu_line_drops());
    CHECK(xlk_inversions() == 0, "ordem do app sem inversão: %s", xlk_inversion_text());
    CHECK(g_shared[0] > 100000, "as duas threads andaram");

    printf("--- 2) detector de ABBA\n");
    pthread_create(&t1, NULL, ab, NULL);
    pthread_join(t1, NULL);
    pthread_create(&t2, NULL, ba, NULL);
    pthread_join(t2, NULL);
    CHECK(xlk_inversions() == 1 && strstr(xlk_inversion_text(), "\"B\" -> \"A\""), "ABBA visto: %s", xlk_inversion_text());

    printf("--- 3) quem espera o quê\n");
    pthread_create(&t1, NULL, holder, NULL);
    pthread_create(&t2, NULL, waiter, NULL);
    usleep(200000);
    char o[300], h[300];
    xlk_waiting_text(5, o, sizeof(o));
    xlk_held_text(h, sizeof(h), 100);
    printf("  %s\n  seguros: %s\n", o, h);
    CHECK(strstr(o, "hw5 espera \"lento\"") && strstr(o, "dono hw4") && strstr(o, "holder pegou") && strstr(o, "waiter pediu"),
          "relatório da espera: %s", o);
    CHECK(strstr(h, "\"lento\" com hw4"), "seguros: %s", h);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    xlk_waiting_text(5, o, sizeof(o));
    CHECK(strstr(o, "não espera") != NULL, "depois: %s", o);

    printf("locks: %d verificações, %d falhas\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
