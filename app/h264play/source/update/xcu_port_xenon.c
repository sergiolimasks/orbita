/*
 * XCloud360 — xcu_port.h no Xbox.
 *   worker: hw XCU_HW_THREAD (3), pilha própria de 256 KB (handshake TLS).
 *   rede:   xcu_net.c (https_conn keep-alive pro x360.pistatrack.com).
 *   pendrive: FatFs direto, no volume que o xc_port_xenon.c já montou
 *     (xc_store_load força o f_mount; ver xcup_prepare). Cada operação
 *     segura o USB só pelo tempo dela (xc_usb_hold), em pedaços de 32 KB, pra
 *     a thread de rede voltar a ler o controle entre um e outro (o menu
 *     continua respondendo na tela de progresso). Nenhum FIL fica aberto
 *     fora de um hold.
 */
#include "xc_update.h"
#include "xcu_port.h"
#include "../login/xc_port.h"
#include "../login/login_task.h"
#include "../login/fatfs/ff.h"

#include <stdint.h>
#include <string.h>
#include <ppc/atomic.h>
#include <xenon_soc/xenon_power.h>

static uint8_t      g_stack[256 * 1024] __attribute__((aligned(128)));
static unsigned int g_lk = 0;

void xcup_lock(void)   { lock(&g_lk); }
void xcup_unlock(void) { unlock(&g_lk); }
void xcup_sync(void)   { __asm__ __volatile__("lwsync" ::: "memory"); }
int  xcup_slot_busy(void) { return xenon_is_thread_task_running(XCU_HW_THREAD) ? 1 : 0; }

int xcup_spawn(void (*fn)(void)) {
    if (xcup_slot_busy()) return -1;
    return xenon_run_thread_task(XCU_HW_THREAD, g_stack + sizeof(g_stack) - 0x100, (void *)fn) == 0 ? 0 : -1;
}

int  xcu_net_get(const char *host, const char *path, const char *headers, size_t max_body,
                 int *status, char **body, size_t *len, char *err, size_t errcap);
void xcu_net_close(void);

int xcup_get(const char *host, const char *path, const char *headers, size_t max_body,
             int *status, char **body, size_t *len, char *err, size_t errcap) {
    return xcu_net_get(host, path, headers, max_body, status, body, len, err, errcap);
}
void xcup_net_close(void) { xcu_net_close(); }

/* ------------------------------------------------------------ pendrive */
#define PIECE (32 * 1024)

static int hold(void) { return xc_usb_hold(1) == 0 ? 0 : -1; }
static void release(void) { xc_usb_hold(0); }

int xcup_prepare(void) {
    uint8_t b;
    (void)xc_store_load("SEED.BIN", &b, 1);   /* efeito colateral: f_mount */
    if (hold()) return -1;
    FILINFO fi;
    FRESULT r = f_stat("XC360", &fi);
    if (r == FR_NO_FILE) r = f_mkdir("XC360");
    release();
    return r == FR_OK ? 0 : -1;   /* FR_NOT_ENABLED = sem pendrive montado */
}

long long xcup_fsize(const char *path) {
    if (hold()) return -1;
    FILINFO fi;
    long long s = f_stat(path, &fi) == FR_OK && !(fi.fattrib & AM_DIR) ? (long long)fi.fsize : -1;
    release();
    return s;
}

int xcup_write_at(const char *path, long long off, const void *b, size_t n) {
    const uint8_t *p = b;
    int first = 1;
    while (n || first) {
        size_t k = n > PIECE ? PIECE : n;
        if (hold()) return -1;
        FIL f;
        FRESULT r = f_open(&f, path, FA_WRITE | (off == 0 && first ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS));
        if (r == FR_OK) {
            if ((long long)f_size(&f) < off) r = FR_INT_ERR;              /* buraco: não deveria */
            if (r == FR_OK) r = f_lseek(&f, (FSIZE_t)off);
            if (r == FR_OK && first && (long long)f_size(&f) > off) r = f_truncate(&f);
            UINT w = 0;
            if (r == FR_OK && k) r = f_write(&f, p, (UINT)k, &w);
            if (r == FR_OK && w != k) r = FR_DENIED;                      /* disco cheio */
            FRESULT c = f_close(&f);
            if (r == FR_OK) r = c;
        }
        release();
        if (r != FR_OK) return -1;
        first = 0;
        p += k;
        off += (long long)k;
        n -= k;
    }
    return 0;
}

long xcup_read(const char *path, long long off, void *b, size_t n) {
    uint8_t *p = b;
    long got = 0;
    while (n) {
        size_t k = n > PIECE ? PIECE : n;
        if (hold()) return -1;
        FIL f;
        UINT rd = 0;
        FRESULT r = f_open(&f, path, FA_READ);
        if (r == FR_OK) {
            r = f_lseek(&f, (FSIZE_t)off);
            if (r == FR_OK) r = f_read(&f, p, (UINT)k, &rd);
            f_close(&f);
        }
        release();
        if (r != FR_OK) return -1;
        got += (long)rd;
        if (rd < k) break;
        p += k;
        off += (long long)k;
        n -= k;
    }
    return got;
}

int xcup_write_file(const char *path, const void *b, size_t n) { return xcup_write_at(path, 0, b, n); }

int xcup_rename(const char *from, const char *to) {
    if (hold()) return -1;
    FRESULT r = f_rename(from, to);
    release();
    return r == FR_OK ? 0 : -1;
}

int xcup_unlink(const char *path) {
    if (hold()) return -1;
    FRESULT r = f_unlink(path);
    release();
    return (r == FR_OK || r == FR_NO_FILE) ? 0 : -1;
}

long long xcup_free_bytes(void) {
    if (hold()) return -1;
    DWORD ncl = 0;
    FATFS *fs = NULL;
    long long b = f_getfree("", &ncl, &fs) == FR_OK && fs ? (long long)ncl * fs->csize * 512 : -1;
    release();
    return b;
}
