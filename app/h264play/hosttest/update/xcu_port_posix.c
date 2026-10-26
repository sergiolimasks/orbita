/*
 * XCloud360 — xcu_port.h no host (pthread) pros testes do xc_update.
 *   pendrive = pasta g_root (xcup_test_root); falhas injetáveis:
 *     xcup_fail_rename_from(n): a n-ésima chamada de rename em diante falha
 *     xcup_fail_write_after(bytes): grava até esse total e depois falha
 *       (pendrive arrancado / energia caiu no meio do download)
 *     xcup_free_override(bytes)
 *   rede = g_get (fetch falso do teste) ou xcu_net_get (HTTPS de verdade).
 */
#include "../../source/update/xcu_port.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_busy = 0;
static void          (*g_fn)(void);
static char            g_root[512] = "/tmp/xc360-update-usb";
static int             g_rename_calls = 0, g_rf_a = 0, g_rf_b = 0;
static long long       g_write_budget = -1, g_free = -2;

typedef int (*xcup_get_fn)(const char *host, const char *path, const char *headers, size_t max_body,
                           int *status, char **body, size_t *len, char *err, size_t errcap);
static xcup_get_fn g_get = NULL;

void xcup_test_root(const char *d) { snprintf(g_root, sizeof(g_root), "%s", d); }
void xcup_test_set_get(xcup_get_fn f) { g_get = f; }
void xcup_test_set_busy(int b) { g_busy = b; }
void xcup_fail_renames(int a, int b) { g_rename_calls = 0; g_rf_a = a; g_rf_b = b; }
void xcup_fail_write_after(long long bytes) { g_write_budget = bytes; }
void xcup_free_override(long long b) { g_free = b; }

void xcup_lock(void)   { pthread_mutex_lock(&g_mx); }
void xcup_unlock(void) { pthread_mutex_unlock(&g_mx); }
void xcup_sync(void)   { __sync_synchronize(); }
int  xcup_slot_busy(void) { return g_busy; }

static void *tramp(void *a) { (void)a; g_fn(); g_busy = 0; return NULL; }

int xcup_spawn(void (*fn)(void)) {
    if (g_busy) return -1;
    g_fn = fn;
    g_busy = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, tramp, NULL) != 0) { g_busy = 0; return -1; }
    pthread_detach(t);
    return 0;
}

int  xcu_net_get(const char *host, const char *path, const char *headers, size_t max_body,
                 int *status, char **body, size_t *len, char *err, size_t errcap) __attribute__((weak));
void xcu_net_close(void) __attribute__((weak));

int xcup_get(const char *host, const char *path, const char *headers, size_t max_body,
             int *status, char **body, size_t *len, char *err, size_t errcap) {
    if (g_get) return g_get(host, path, headers, max_body, status, body, len, err, errcap);
    if (xcu_net_get) return xcu_net_get(host, path, headers, max_body, status, body, len, err, errcap);
    snprintf(err, errcap, "sem rede no teste");
    return -1;
}
void xcup_net_close(void) { if (!g_get && xcu_net_close) xcu_net_close(); }

static void full(char *o, size_t n, const char *p) { snprintf(o, n, "%s/%s", g_root, p); }

int xcup_prepare(void) {
    char p[600];
    mkdir(g_root, 0755);
    full(p, sizeof(p), "XC360");
    return (mkdir(p, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

long long xcup_fsize(const char *path) {
    char p[600];
    struct stat st;
    full(p, sizeof(p), path);
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
}

int xcup_write_at(const char *path, long long off, const void *b, size_t n) {
    char p[600];
    full(p, sizeof(p), path);
    FILE *f = fopen(p, off == 0 ? "wb" : "r+b");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    if (sz < off) { fclose(f); return -1; }
    if (sz > off && ftruncate(fileno(f), off) != 0) { fclose(f); return -1; }
    fseek(f, off, SEEK_SET);
    size_t k = n;
    int fail = 0;
    if (g_write_budget >= 0) {
        if ((long long)k > g_write_budget) { k = (size_t)g_write_budget; fail = 1; }
        g_write_budget -= (long long)k;
    }
    size_t w = fwrite(b, 1, k, f);
    if (fclose(f) != 0 || w != k || fail) return -1;
    return 0;
}

long xcup_read(const char *path, long long off, void *b, size_t n) {
    char p[600];
    full(p, sizeof(p), path);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, off, SEEK_SET);
    size_t r = fread(b, 1, n, f);
    fclose(f);
    return (long)r;
}

int xcup_write_file(const char *path, const void *b, size_t n) { return xcup_write_at(path, 0, b, n); }

int xcup_rename(const char *from, const char *to) {
    char a[600], b[600];
    g_rename_calls++;
    if (g_rf_a && g_rename_calls >= g_rf_a && (!g_rf_b || g_rename_calls <= g_rf_b)) return -1;
    full(a, sizeof(a), from);
    full(b, sizeof(b), to);
    if (access(b, F_OK) == 0) return -1;   /* como o f_rename: destino existe = FR_EXIST */
    return rename(a, b) == 0 ? 0 : -1;
}

int xcup_unlink(const char *path) {
    char p[600];
    full(p, sizeof(p), path);
    return (unlink(p) == 0 || errno == ENOENT) ? 0 : -1;
}

long long xcup_free_bytes(void) { return g_free == -2 ? 1LL << 34 : g_free; }
