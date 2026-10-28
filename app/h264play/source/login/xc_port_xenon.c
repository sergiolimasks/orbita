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
#include "../xc_wdog.h"
#include "../xc_lock.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <ppc/timebase.h>
#include <ppc/atomic.h>
#include <xenon_soc/xenon_secotp.h>
#include <network/network.h>

#ifndef XC_BUILD_EPOCH
#define XC_BUILD_EPOCH 1792800000LL
#endif

/* ------------------------------------------------------------------ log */

/* v0.21.9: a UART (115200) custa ~87 us por caractere e a printf espera cada
 * um: uma linha de 200 B segurava a hw1 (rede/Wi-Fi/USB) por ~17 ms, e o dump
 * do RX parado por ~0,3 s. Na hw1 a linha vai pra um anel e o main manda pra
 * UART aos poucos (xc_log_uart_drain). Os logs em RAM (beta, WIFILOG) seguem
 * na hora, iguais. */
#define ULOG_RING 16384
static char         g_ulog[ULOG_RING];
static size_t       g_ulog_len = 0;
static unsigned     g_ulog_lost = 0;
static xlk_t        g_ulog_lk = XLK_INIT("UART hw1");

static int on_hw1(void) {
    unsigned v;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(v));
    return v == 1;
}

void xc_log_uart_drain(void) {
    static char out[1024 + 64];
    size_t n = 0;
    unsigned lost;
    xlk_lock(&g_ulog_lk, "uart drain");
    if (g_ulog_len) {
        n = g_ulog_len > 192 ? 192 : g_ulog_len;   /* ~17 ms de UART por volta do menu */
        /* corta no fim de uma linha quando dá */
        if (n < g_ulog_len) {
            size_t k = n;
            while (k > 0 && g_ulog[k - 1] != '\n') k--;
            if (k > 0) n = k;
        }
        memcpy(out, g_ulog, n);
        memmove(g_ulog, g_ulog + n, g_ulog_len - n);
        g_ulog_len -= n;
    }
    lost = g_ulog_lost;
    g_ulog_lost = 0;
    xlk_unlock(&g_ulog_lk);
    if (lost) printf("(%u linhas da hw1 perdidas na UART: anel cheio)\n", lost);
    if (n) { out[n] = 0; printf("%s", out); }
}

volatile uint64_t xc_log_tb[8];   /* v0.21.25: tempo dentro do xc_log por hw thread ([cpu]) */
static void xc_log_(const char *fmt, va_list ap0);
void xc_log(const char *fmt, ...) {
    uint64_t t0 = mftb();
    unsigned pir;
    va_list ap;
    va_start(ap, fmt);
    xc_log_(fmt, ap);
    va_end(ap);
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(pir));
    xc_log_tb[pir & 7] += mftb() - t0;
}
static void xc_log_(const char *fmt, va_list ap0) {
    char buf[200];
    va_list ap;
    va_copy(ap, ap0);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (on_hw1()) {
        size_t l = strlen(buf);
        if (!xlk_try(&g_ulog_lk, "xc_log hw1", 2000)) { g_ulog_lost++; goto ram; }
        if (g_ulog_len + l + 1 <= ULOG_RING) {
            memcpy(g_ulog + g_ulog_len, buf, l);
            g_ulog[g_ulog_len + l] = '\n';
            g_ulog_len += l + 1;
        } else {
            g_ulog_lost++;
        }
        xlk_unlock(&g_ulog_lk);
    } else {
        printf("%s\n", buf);
    }
ram:
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
int  fat_diskio_raw_write(uint32_t sector, const void *buf512);
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

/* v0.21.28: pendrive que recusa escrita (testadores: o 1º f_mkdir do XC360
 * num pendrive novo travava o console no tinyehci; corrigido no libxenon).
 * Erro de disco 3 vezes seguidas = não grava mais nada até reiniciar: cada
 * tentativa custa timeouts de USB e o app segue igual (a chave beta fica na
 * RAM, os logs vão pela rede). */
static int g_fs_wfail = 0;
static int fs_write_ok(void) { return g_fs_wfail < 3; }
/* v0.21.37: tela de atualização (sem IO; o boot já montou ou desistiu) */
int xc_store_writable(void) { return g_fs_ok && fs_write_ok(); }
static void fs_note(FRESULT r) {
    if (r == FR_OK || r == FR_EXIST) { g_fs_wfail = 0; return; }
    if (r == FR_DISK_ERR || r == FR_INT_ERR || r == FR_NOT_READY || r == FR_WRITE_PROTECTED) {
        if (++g_fs_wfail == 3)
            xc_log("[usb] o pendrive recusou a escrita 3 vezes seguidas (erro %d): nada mais é gravado até reiniciar", (int)r);
    }
}

static void tmp_of(char *o, size_t n, const char *name) {
    char base[16];
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strchr(base, '.');
    if (dot) *dot = 0;
    snprintf(o, n, STORE_DIR "/%s.NEW", base);
}

/* v0.21.30: cópia em RAM do TOKEN.TXT. Pendrive que não grava (alguns modelos
 * baratos) deixava o login sem token na hora do jogo ("Faca o login no Xbox
 * primeiro"); agora o que foi gravado (ou tentado) vale até reiniciar. */
#define TOKMEM_CAP 4096
static char   g_tokmem[TOKMEM_CAP];
static size_t g_tokmem_n = 0;
static int is_token_file(const char *name) { return name && !strcmp(name, "TOKEN.TXT"); }
static int xc_store_load_file(const char *name, void *buf, size_t cap);
static int xc_store_save_file(const char *name, const void *buf, size_t n);

int xc_store_load(const char *name, void *buf, size_t cap) {
    int r = xc_store_load_file(name, buf, cap);
    if (r <= 0 && is_token_file(name) && g_tokmem_n && g_tokmem_n <= cap) {
        memcpy(buf, g_tokmem, g_tokmem_n);
        xc_log("[usb] TOKEN.TXT do pendrive indisponivel: usando a copia da memoria");
        return (int)g_tokmem_n;
    }
    return r;
}

static int xc_store_load_file(const char *name, void *buf, size_t cap) {
    int res = -1;
    xwd_beat2("pendrive: lendo", name);
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
    if (is_token_file(name)) {
        if (n < TOKMEM_CAP) { memcpy(g_tokmem, buf, n); g_tokmem_n = n; }
        else g_tokmem_n = 0;
    }
    return xc_store_save_file(name, buf, n);
}

static int xc_store_save_file(const char *name, const void *buf, size_t n) {
    int res = -1;
    xwd_beat2("pendrive: gravando", name);
    if (xc_usb_hold(1) != 0) {
        xc_log("[usb] rede nao liberou o USB, pulando o pendrive");
        return -1;
    }
    if (!fs_write_ok()) { xc_usb_hold(0); return -1; }
    if (fs_mount() == 0) {
        char p[32], t[32];
        FIL f;
        path_of(p, sizeof(p), name);
        tmp_of(t, sizeof(t), name);
        FRESULT rmk = f_mkdir(STORE_DIR);   /* FR_EXIST é ok */
        fs_note(rmk);
        if (!fs_write_ok()) { xc_usb_hold(0); return -1; }
        {   /* "COVERS/1A2B3C4D.JPG" (xc_catalog): cria a subpasta (8.3) */
            const char *sl = strchr(name, '/');
            if (sl && (size_t)(sl - name) <= 8) {
                char d[24];
                snprintf(d, sizeof(d), STORE_DIR "/%.*s", (int)(sl - name), name);
                f_mkdir(d);
            }
        }
        /* grava no .NEW e troca: queda de energia no meio não corrompe o atual */
        FRESULT ro = f_open(&f, t, FA_WRITE | FA_CREATE_ALWAYS);
        fs_note(ro);
        if (ro == FR_OK) {
            UINT w = 0;
            FRESULT r = f_write(&f, buf, (UINT)n, &w);
            FRESULT r2 = f_close(&f);
            fs_note(r != FR_OK ? r : r2);
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

/* v0.21.5: acrescenta no fim; passou de cap bytes = o atual vira old_name
 * (o old anterior é apagado) e começa um novo. 0 = ok. */
int xc_store_append(const char *name, const char *old_name, const void *buf, size_t n, size_t cap) {
    int res = -1;
    xwd_beat2("pendrive: acrescentando", name);
    if (!fs_write_ok()) return -1;   /* v0.21.28 */
    if (xc_usb_hold(1) != 0) return -1;
    if (fs_mount() == 0) {
        char p[32], o[32];
        FIL f;
        path_of(p, sizeof(p), name);
        path_of(o, sizeof(o), old_name);
        f_mkdir(STORE_DIR);
        FILINFO fi;
        if (f_stat(p, &fi) == FR_OK && fi.fsize + n > cap) {
            f_unlink(o);
            f_rename(p, o);
        }
        if (f_open(&f, p, FA_WRITE | FA_OPEN_ALWAYS) == FR_OK) {
            UINT w = 0;
            if (f_lseek(&f, f_size(&f)) == FR_OK && f_write(&f, buf, (UINT)n, &w) == FR_OK && w == n) res = 0;
            if (f_close(&f) != FR_OK) res = -1;
        }
    }
    xc_usb_hold(0);
    return res;
}

int xc_store_remove(const char *name) {
    if (is_token_file(name)) { memset(g_tokmem, 0, sizeof(g_tokmem)); g_tokmem_n = 0; }
    int res = -1;
    xwd_beat2("pendrive: apagando", name);
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


/* ------------------------------------------------------------- HANG.TXT
 * v0.21.7: registro de travamento que não depende de FatFs nem do
 * xc_usb_hold. No boot (main, antes da thread de rede) o XC360/HANG.TXT é
 * criado com 64 KB (128 setores de texto) e conferido contíguo; daí em diante
 * o vigia escreve 1 setor por registro direto no disco (fat_diskio_raw_write),
 * em anel a partir do setor 1, continuando do maior número da sessão anterior
 * (o travamento de ontem não é apagado pelo boot de hoje). */
#define HANG_FILE  "HANG.TXT"
#define HANG_BYTES (64 * 1024)
#define HANG_SECT  (HANG_BYTES / 512)
static uint32_t     g_hang_lba = 0;
static unsigned     g_hang_seq = 0, g_hang_next = 1;
static int          g_hang_ok = 0;
static volatile int g_hang_busy = 0;

int xc_hang_busy(void) { return g_hang_busy; }

/* v0.21.30: os registros de travamento ficavam só no pendrive do testador.
 * No boot seguinte os mais novos ainda não enviados (> XC360/HANGSEEN.TXT)
 * vão pro log beta como "[wdog] registro anterior #N". Máx. HANG_UP_MAX. */
#define HANG_UP_MAX 6
static char     g_hang_up[HANG_UP_MAX][512];
static unsigned g_hang_up_q[HANG_UP_MAX];
static int      g_hang_up_n = 0;
static void hang_keep(unsigned q, const char *sec) {
    int slot = -1;
    if (g_hang_up_n < HANG_UP_MAX) slot = g_hang_up_n++;
    else {   /* troca o mais velho se este for mais novo */
        int old = 0;
        for (int k = 1; k < HANG_UP_MAX; k++) if (g_hang_up_q[k] < g_hang_up_q[old]) old = k;
        if (g_hang_up_q[old] < q) slot = old;
    }
    if (slot < 0) return;
    g_hang_up_q[slot] = q;
    memcpy(g_hang_up[slot], sec, 511);
    g_hang_up[slot][511] = 0;
}
static void hang_report_unsent(void) {
    char t[24];
    unsigned seen = 0;
    int n = xc_store_load_file("HANGSEEN.TXT", t, sizeof(t) - 1);
    if (n > 0) { t[n] = 0; seen = (unsigned)strtoul(t, NULL, 10); }
    unsigned top = seen;
    for (int pass = 0; pass < g_hang_up_n; pass++) {   /* em ordem crescente */
        int best = -1;
        for (int k = 0; k < g_hang_up_n; k++)
            if (g_hang_up_q[k] > seen && (best < 0 || g_hang_up_q[k] < g_hang_up_q[best])) best = k;
        if (best < 0) break;
        char *s = g_hang_up[best];
        size_t L = strlen(s);
        while (L > 0 && (s[L - 1] == ' ' || s[L - 1] == '\n')) s[--L] = 0;
        for (size_t o = 0; o < L; o += 170)
            xc_log("[wdog] registro anterior %s%.170s", o ? "(cont.) " : "", s + o);
        if (g_hang_up_q[best] > top) top = g_hang_up_q[best];
        seen = g_hang_up_q[best];
    }
    if (top > 0) {
        n = snprintf(t, sizeof(t), "%u", top);
        if (n > 0) xc_store_save_file("HANGSEEN.TXT", t, (size_t)n);
    }
}

int xc_hang_prepare(const char *version) {
    static char sec[512];
    int ok = -1;
    if (xc_usb_hold(1) != 0) return -1;
    if (fs_mount() == 0) {
        char p[32];
        FIL f;
        path_of(p, sizeof(p), HANG_FILE);
        f_mkdir(STORE_DIR);
        if (f_open(&f, p, FA_READ | FA_WRITE | FA_OPEN_ALWAYS) == FR_OK) {
            int fresh = f_size(&f) != HANG_BYTES;
            if (fresh) {
                f_lseek(&f, 0);
                f_truncate(&f);
                memset(sec, ' ', sizeof(sec));
                sec[511] = '\n';
                UINT w;
                for (int i = 0; i < HANG_SECT; i++) f_write(&f, sec, 512, &w);
            } else {
                /* continua depois do maior registro "#N" */
                UINT r;
                f_lseek(&f, 512);
                for (unsigned i = 1; i < HANG_SECT; i++) {
                    if (f_read(&f, sec, 512, &r) != FR_OK || r != 512) break;
                    if (sec[0] == '#') {
                        unsigned q = (unsigned)strtoul(sec + 1, NULL, 10);
                        if (q >= g_hang_seq) { g_hang_seq = q; g_hang_next = i % (HANG_SECT - 1) + 1; }
                        hang_keep(q, sec);
                    }
                }
            }
            memset(sec, ' ', sizeof(sec));
            int n = snprintf(sec, sizeof(sec),
                             "orbita %s - travamentos ([wdog]). 1 registro por setor (#numero), em anel; "
                             "o maior numero e o mais novo.", version ? version : "?");
            if (n > 0 && n < 511) sec[n] = ' ';
            sec[510] = '\n';
            sec[511] = '\n';
            UINT w;
            f_lseek(&f, 0);
            f_write(&f, sec, 512, &w);
            f_sync(&f);
            /* contíguo? (a escrita direta precisa) */
            FATFS *fs = f.obj.fs;
            DWORD cs = fs->csize, first = f.obj.sclust;
            int contig = first >= 2;
            for (DWORD ofs = 0; contig && ofs < HANG_BYTES; ofs += cs * 512u) {
                if (f_lseek(&f, ofs + 1) != FR_OK) { contig = 0; break; }
                if (f.clust != first + ofs / (cs * 512u)) contig = 0;
            }
            if (contig) {
                g_hang_lba = (uint32_t)(fs->database + (LBA_t)(first - 2) * cs);
                g_hang_ok = 1;
                ok = 0;
            }
            f_close(&f);
            xc_log("[wdog] %s %s: %s (próximo registro #%u)", HANG_FILE, fresh ? "criado" : "aberto",
                   contig ? "contíguo, escrita direta pronta" : "NÃO contíguo, só o log normal", g_hang_seq + 1);
        }
    }
    xc_usb_hold(0);
    hang_report_unsent();   /* v0.21.30 (fora do hold: as funções do pendrive pegam o hold sozinhas) */
    return ok;
}

/* Quem chama decide se pode (ninguém no tinyehci, ver xc_wdog.c). */
int xc_hang_write(const char *line) {
    static char sec[512];
    if (!g_hang_ok || __sync_lock_test_and_set(&g_hang_busy, 1)) return -1;
    long long t = xc_time_now();
    memset(sec, ' ', sizeof(sec));
    int n = snprintf(sec, sizeof(sec), "#%u t=%llu unix=%lld %s", g_hang_seq + 1,
                     (unsigned long long)(mftb() / PPC_TIMEBASE_FREQ), t, line);
    if (n > 0 && n < 511) sec[n] = ' ';
    sec[511] = '\n';
    int r = fat_diskio_raw_write(g_hang_lba + g_hang_next, sec);
    if (r == 0) {
        g_hang_seq++;
        g_hang_next = g_hang_next % (HANG_SECT - 1) + 1;
    }
    __sync_lock_release(&g_hang_busy);
    return r;
}

/* ----------------------------------------------------------- NETLOG.TXT
 * v0.21.10: o WIFILOG.TXT só era gravado pelo worker das capas (FatFs, hw 3).
 * Numa sessão a hw 3 é da sessão/decoder e o log morria justo quando
 * importava. O XC360/NETLOG.TXT é pré-alocado no boot (512 KB, contíguo,
 * como o HANG.TXT) e a hw1 escreve 1 setor por vez direto no disco, em anel
 * a partir do setor 1, com "#N" no começo de cada setor (o maior = o mais
 * novo). Sem FatFs e sem xc_usb_hold: a hw1 já é a dona do USB. */
#define NETLOG_FILE  "NETLOG.TXT"
#define NETLOG_BYTES (512 * 1024)
#define NETLOG_SECT  (NETLOG_BYTES / 512)
static uint32_t     g_nl_lba = 0;
static unsigned     g_nl_seq = 0, g_nl_next = 1;
static int          g_nl_ok = 0;
static unsigned     g_nl_fail = 0, g_nl_writes = 0;
static uint32_t     g_nl_worst_us = 0;

int xc_netlog_prepare(const char *version) {
    static char sec[512];
    int ok = -1;
    if (xc_usb_hold(1) != 0) return -1;
    if (fs_mount() == 0) {
        char p[32];
        FIL f;
        path_of(p, sizeof(p), NETLOG_FILE);
        f_mkdir(STORE_DIR);
        if (f_open(&f, p, FA_READ | FA_WRITE | FA_OPEN_ALWAYS) == FR_OK) {
            int fresh = f_size(&f) != NETLOG_BYTES;
            if (fresh) {
                f_lseek(&f, 0);
                f_truncate(&f);
                memset(sec, ' ', sizeof(sec));
                sec[511] = '\n';
                UINT w;
                for (int i = 0; i < NETLOG_SECT; i++) f_write(&f, sec, 512, &w);
            } else {
                UINT r;
                f_lseek(&f, 512);
                for (unsigned i = 1; i < NETLOG_SECT; i++) {
                    if (f_read(&f, sec, 512, &r) != FR_OK || r != 512) break;
                    if (sec[0] == '#') {
                        unsigned q = (unsigned)strtoul(sec + 1, NULL, 10);
                        if (q >= g_nl_seq) { g_nl_seq = q; g_nl_next = i % (NETLOG_SECT - 1) + 1; }
                    }
                }
            }
            memset(sec, ' ', sizeof(sec));
            int n = snprintf(sec, sizeof(sec),
                             "orbita %s - log da rede gravado direto no disco (sessao/jogo). 1 setor por bloco "
                             "(#numero), em anel; o maior numero e o mais novo. Ordene pelos numeros.",
                             version ? version : "?");
            if (n > 0 && n < 511) sec[n] = ' ';
            sec[510] = '\n';
            sec[511] = '\n';
            UINT w;
            f_lseek(&f, 0);
            f_write(&f, sec, 512, &w);
            f_sync(&f);
            FATFS *fs = f.obj.fs;
            DWORD cs = fs->csize, first = f.obj.sclust;
            int contig = first >= 2;
            for (DWORD ofs = 0; contig && ofs < NETLOG_BYTES; ofs += cs * 512u) {
                if (f_lseek(&f, ofs + 1) != FR_OK) { contig = 0; break; }
                if (f.clust != first + ofs / (cs * 512u)) contig = 0;
            }
            if (contig) {
                g_nl_lba = (uint32_t)(fs->database + (LBA_t)(first - 2) * cs);
                g_nl_ok = 1;
                ok = 0;
            }
            f_close(&f);
            xc_log("[wdog] %s %s: %s (próximo bloco #%u)", NETLOG_FILE, fresh ? "criado" : "aberto",
                   contig ? "contíguo, escrita direta pronta" : "NÃO contíguo, sessão sem log no pendrive", g_nl_seq + 1);
        }
    }
    xc_usb_hold(0);
    return ok;
}

int xc_netlog_ready(void) { return g_nl_ok; }

/* um setor: "#N t=.. " + texto (cortado em ~490 B). Só quem é dono do USB
 * (a hw1 fora de um xc_usb_hold). 0 = gravou. */
int xc_netlog_write(const char *text, size_t n) {
    static char sec[512];
    if (!g_nl_ok || __sync_lock_test_and_set(&g_hang_busy, 1)) return -1;
    memset(sec, ' ', sizeof(sec));
    int h = snprintf(sec, sizeof(sec), "#%u t=%llu ", g_nl_seq + 1, (unsigned long long)(mftb() / PPC_TIMEBASE_FREQ));
    if (h < 0) h = 0;
    if (n > (size_t)(510 - h)) n = (size_t)(510 - h);
    memcpy(sec + h, text, n);
    sec[510] = '\n';
    sec[511] = '\n';
    uint64_t t0 = mftb();
    int r = fat_diskio_raw_write(g_nl_lba + g_nl_next, sec);
    uint32_t us = (uint32_t)((mftb() - t0) / (PPC_TIMEBASE_FREQ / 1000000ULL));
    if (us > g_nl_worst_us) g_nl_worst_us = us;
    if (r == 0) {
        g_nl_seq++;
        g_nl_writes++;
        g_nl_next = g_nl_next % (NETLOG_SECT - 1) + 1;
    } else {
        g_nl_fail++;
    }
    __sync_lock_release(&g_hang_busy);
    return r;
}

void xc_netlog_stats(unsigned *writes, unsigned *fails, unsigned *worst_us) {
    *writes = g_nl_writes;
    *fails = g_nl_fail;
    *worst_us = g_nl_worst_us;
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
