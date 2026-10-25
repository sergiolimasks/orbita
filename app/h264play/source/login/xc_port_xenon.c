/*
 * XCloud360 v0.17 — xc_port.h no Xbox (o transporte TCP/DNS fica em
 * net_pipe.c). Aqui: log, hora, arquivos no pendrive (FatFs), entropia.
 *
 * HORA: o libxenon lê o RTC do SMC no gettimeofday() (newlib, msg 0x04,
 * base fixa "RTC_BASE"), mas a base é chutada e o RTC de um RGH costuma
 * estar zerado/errado. Não confiamos nele pra validade de certificado: ele só
 * é logado. A hora "de verdade" vem do header Date da 1ª resposta HTTPS (ver
 * https.h), e daí avança pelo timebase (mftb, PPC_TIMEBASE_FREQ).
 *
 * ENTROPIA: o libxenon não expõe RNG de hardware (crypt/ só tem des/rc4/
 * sha1/hmac). mbedtls_hardware_poll = jitter do timebase medindo rajadas de
 * acessos aleatórios a 2MB de RAM (falta de cache, refresh de DRAM, a thread
 * irmã de SMT e a thread de rede mexendo no mesmo núcleo/barramento), 16
 * amostras por byte, condicionado pelo SHA-512 do entropy.c. Personalização
 * do DRBG (xc_seed_material): fusíveis do console (secotp, únicos por Xbox),
 * MAC, RTC, timebase e a seed gravada no pendrive no boot anterior
 * (XC360/SEED.BIN, regravada a cada login com bytes do DRBG).
 */
#include "xc_port.h"
#include "net_pipe.h"
#include "login_task.h"
#include "fatfs/ff.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <ppc/timebase.h>
#include <xenon_soc/xenon_secotp.h>
#include <network/network.h>

#ifndef XC_BUILD_EPOCH
#define XC_BUILD_EPOCH 1792800000LL
#endif

/* ------------------------------------------------------------------ log */

void xc_log(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    np_log_line(buf);
}

/* ----------------------------------------------------------------- hora */

static volatile long long g_unix_base = -1;
static volatile uint64_t  g_tb_base = 0;

long long xc_time_floor(void) { return XC_BUILD_EPOCH; }

long long xc_time_now(void) {
    long long b = g_unix_base;
    if (b < 0) return -1;
    return b + (long long)((mftb() - g_tb_base) / PPC_TIMEBASE_FREQ);
}

static long long rtc_unix(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return -1;
    return (long long)tv.tv_sec;
}

static void fmt_time(char *o, size_t n, long long t) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (t < 0 || !gmtime_r(&tt, &tm)) { snprintf(o, n, "?"); return; }
    snprintf(o, n, "%04d-%02d-%02d %02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void xc_time_set(long long unix_now, const char *source) {
    g_tb_base = mftb();
    __asm__ __volatile__("lwsync" ::: "memory");
    g_unix_base = unix_now;
    char a[32], b[32];
    long long rtc = rtc_unix();
    fmt_time(a, sizeof(a), unix_now);
    fmt_time(b, sizeof(b), rtc);
    xc_log("[hora] %s pelo Date de %s (RTC do Xbox diz %s, diferenca %llds)", a, source, b,
           rtc >= 0 ? rtc - unix_now : 0);
}

void xc_time_log_rtc(void) {
    char b[32];
    long long rtc = rtc_unix();
    fmt_time(b, sizeof(b), rtc);
    xc_log("[hora] RTC do Xbox: %s (nao usado pra certificados; hora vem do Date)", b);
}

/* -------------------------------------------------------------- arquivos */

int  fat_diskio_sectors(int dev);
void fat_diskio_select(int dev);

#define STORE_DIR "XC360"
static FATFS g_fs;
static int   g_fs_ok = 0, g_fs_tried = 0;

/* Com o USB seguro (xc_usb_hold). */
static int fs_mount(void) {
    if (g_fs_ok) return 0;
    if (g_fs_tried >= 3) return -1;   /* sem pendrive: não insiste toda hora */
    g_fs_tried++;
    for (int d = 0; d < 3; d++) {
        int sec = fat_diskio_sectors(d);
        if (sec <= 0) continue;
        fat_diskio_select(d);
        FRESULT r = f_mount(&g_fs, "", 1);
        if (r == FR_OK) {
            g_fs_ok = 1;
            xc_log("[usb] pendrive FAT montado (dispositivo %d, %d setores)", d, sec);
            return 0;
        }
        xc_log("[usb] dispositivo %d: f_mount falhou (%d)", d, (int)r);
    }
    xc_log("[usb] nenhum pendrive FAT encontrado");
    return -1;
}

static void path_of(char *o, size_t n, const char *name) { snprintf(o, n, STORE_DIR "/%s", name); }

static void tmp_of(char *o, size_t n, const char *name) {
    char base[16];
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strchr(base, '.');
    if (dot) *dot = 0;
    snprintf(o, n, STORE_DIR "/%s.NEW", base);
}

int xc_store_load(const char *name, void *buf, size_t cap) {
    int res = -1;
    if (xc_usb_hold(1) != 0) {
        xc_log("[usb] rede nao liberou o USB, pulando o pendrive");
        return -1;
    }
    if (fs_mount() == 0) {
        char p[32];
        FIL f;
        path_of(p, sizeof(p), name);
        if (f_open(&f, p, FA_READ) == FR_OK) {
            UINT got = 0;
            if (f_read(&f, buf, (UINT)cap, &got) == FR_OK) res = (int)got;
            f_close(&f);
        }
    }
    xc_usb_hold(0);
    return res;
}

int xc_store_save(const char *name, const void *buf, size_t n) {
    int res = -1;
    if (xc_usb_hold(1) != 0) {
        xc_log("[usb] rede nao liberou o USB, pulando o pendrive");
        return -1;
    }
    if (fs_mount() == 0) {
        char p[32], t[32];
        FIL f;
        path_of(p, sizeof(p), name);
        tmp_of(t, sizeof(t), name);
        f_mkdir(STORE_DIR);   /* FR_EXIST é ok */
        /* grava no .NEW e troca: queda de energia no meio não corrompe o atual */
        if (f_open(&f, t, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT w = 0;
            FRESULT r = f_write(&f, buf, (UINT)n, &w);
            FRESULT r2 = f_close(&f);
            if (r == FR_OK && r2 == FR_OK && w == n) {
                f_unlink(p);
                if (f_rename(t, p) == FR_OK) res = 0;
            }
        }
        if (res != 0) xc_log("[usb] falha gravando %s", p);
    }
    xc_usb_hold(0);
    return res;
}

int xc_store_remove(const char *name) {
    int res = -1;
    if (xc_usb_hold(1) != 0) {
        xc_log("[usb] rede nao liberou o USB, pulando o pendrive");
        return -1;
    }
    if (fs_mount() == 0) {
        char p[32];
        path_of(p, sizeof(p), name);
        res = f_unlink(p) == FR_OK ? 0 : -1;
    }
    xc_usb_hold(0);
    return res;
}

/* ------------------------------------------------------------- entropia */

#define JIT_BUF (2 * 1024 * 1024)
static uint8_t  g_jit[JIT_BUF] __attribute__((aligned(128)));
static uint32_t g_jit_x = 0x9E3779B9u;

static uint32_t jitter_sample(void) {
    uint64_t t0 = mftb();
    uint32_t x = g_jit_x;
    for (int i = 0; i < 48; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        g_jit[x & (JIT_BUF - 1)] += (uint8_t)(i + (t0 & 0xFF));
    }
    uint64_t t1 = mftb();
    g_jit_x = x ^ (uint32_t)t1;
    return (uint32_t)(t1 - t0);
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    for (size_t i = 0; i < len; i++) {
        uint32_t v = 0;
        for (int k = 0; k < 16; k++) {
            uint32_t d = jitter_sample();
            v = (v << 3 | v >> 29) ^ d ^ (d >> 8);
        }
        output[i] = (uint8_t)(v ^ v >> 8 ^ v >> 16 ^ v >> 24);
    }
    *olen = len;
    return 0;
}

size_t xc_seed_material(uint8_t *out, size_t cap) {
    size_t n = 0;
#define PUT(p, l) do { size_t _l = (l); if (n + _l <= cap) { memcpy(out + n, (p), _l); n += _l; } } while (0)
    for (int i = 0; i < 12; i++) {
        uint64_t f = xenon_secotp_read_line(i);
        PUT(&f, sizeof(f));
    }
    PUT(netif.hwaddr, 6);
    long long rtc = rtc_unix();
    PUT(&rtc, sizeof(rtc));
    uint64_t tb = mftb();
    PUT(&tb, sizeof(tb));
    uint8_t seed[32];
    int sl = xc_store_load("SEED.BIN", seed, sizeof(seed));
    if (sl > 0) PUT(seed, (size_t)sl);
    xc_log("[rng] entropia: jitter do timebase + fuses/MAC/RTC%s", sl == 32 ? " + seed do pendrive" : " (sem seed salva)");
    memset(seed, 0, sizeof(seed));
    tb = mftb();
    PUT(&tb, sizeof(tb));
#undef PUT
    return n;
}
