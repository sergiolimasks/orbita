/* orbita v0.21.28 — o caminho do 1º save num pendrive novo, no host:
 * FatFs de verdade (source/login/fatfs/ff.c) + o nosso fat_diskio.c de
 * verdade, sobre um "usb2mass_ops_0" falso que lê/grava uma imagem FAT32
 * criada pelo mkfs.fat. Os testadores travavam no f_mkdir("XC360") (o
 * tinyehci derrubava o console num erro de escrita; corrigido no libxenon).
 *   1) imagem nova: f_mkdir("XC360") + arquivo dentro, relido igual;
 *   2) o pendrive recusando escrita: f_mkdir devolve FR_DISK_ERR (sem
 *      crash), o volume continua legível;
 *   3) a limpeza do cluster do diretório passa pelo buffer de 8 setores em
 *      várias escritas (o caminho que travava). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatfs/ff.h"
#include <diskio/disc_io.h>

void fat_diskio_select(int dev);
int fat_diskio_sectors(int dev);

static uint8_t *g_img;
static uint32_t g_nsec;
static int g_fail_writes = 0;
static unsigned g_writes = 0, g_write_secs = 0, g_max_chunk = 0;

static bool m_startup(void) { return true; }
static bool m_inserted(void) { return true; }
static bool m_read(sec_t s, sec_t n, void *b) {
    if ((uint64_t)s + n > g_nsec) return false;
    memcpy(b, g_img + (size_t)s * 512, (size_t)n * 512);
    return true;
}
static bool m_write(sec_t s, sec_t n, const void *b) {
    if (g_fail_writes) return false;   /* o pendrive dá STALL na escrita */
    if ((uint64_t)s + n > g_nsec) return false;
    memcpy(g_img + (size_t)s * 512, b, (size_t)n * 512);
    g_writes++;
    g_write_secs += n;
    if (n > g_max_chunk) g_max_chunk = n;
    return true;
}
static bool m_clear(void) { return true; }
static bool m_shutdown(void) { return true; }
static s32 m_sectors(void) { return (s32)g_nsec; }

DISC_INTERFACE usb2mass_ops_0 = { 0, 3, m_startup, m_inserted, m_read, m_write, m_clear, m_shutdown, m_sectors };
DISC_INTERFACE usb2mass_ops_1 = { 0, 0, m_startup, m_inserted, m_read, m_write, m_clear, m_shutdown, m_sectors };
DISC_INTERFACE usb2mass_ops_2 = { 0, 0, m_startup, m_inserted, m_read, m_write, m_clear, m_shutdown, m_sectors };

static int g_checks = 0, g_bad = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_bad++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(g_img);
    g_img = malloc((size_t)n);
    if (fread(g_img, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    g_nsec = (uint32_t)(n / 512);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: fatfs_test <imagem FAT>\n"); return 2; }
    FATFS fs;
    FIL fl;
    UINT w = 0, r = 0;
    char buf[128];

    printf("--- 1) pendrive novo: f_mkdir(XC360) + arquivo\n");
    CHECK(load(argv[1]) == 0, "imagem");
    CHECK(fat_diskio_sectors(0) == (int)g_nsec, "setores");
    fat_diskio_select(0);
    CHECK(f_mount(&fs, "", 1) == FR_OK, "montou");
    FRESULT rm = f_mkdir("XC360");
    CHECK(rm == FR_OK, "f_mkdir XC360 = %d", (int)rm);
    CHECK(f_mkdir("XC360") == FR_EXIST, "de novo: FR_EXIST");
    CHECK(f_open(&fl, "XC360/BETAKEY.TXT", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "abriu");
    CHECK(f_write(&fl, "ABCD-EFGH-1234", 14, &w) == FR_OK && w == 14, "gravou");
    CHECK(f_close(&fl) == FR_OK, "fechou");
    CHECK(f_open(&fl, "XC360/BETAKEY.TXT", FA_READ) == FR_OK && f_read(&fl, buf, sizeof(buf), &r) == FR_OK && r == 14 &&
          !memcmp(buf, "ABCD-EFGH-1234", 14), "releu igual");
    f_close(&fl);
    printf("  escritas %u (%u setores, maior pedaço %u)\n", g_writes, g_write_secs, g_max_chunk);
    CHECK(g_max_chunk <= 8, "pelo buffer de 8 setores (%u)", g_max_chunk);
    f_mount(NULL, "", 0);

    printf("--- 2) pendrive recusando escrita\n");
    CHECK(load(argv[1]) == 0, "imagem nova");
    fat_diskio_select(0);
    CHECK(f_mount(&fs, "", 1) == FR_OK, "montou (só leitura)");
    g_fail_writes = 1;
    rm = f_mkdir("XC360");
    CHECK(rm == FR_DISK_ERR, "f_mkdir com escrita recusada = FR_DISK_ERR (%d), sem crash", (int)rm);
    FRESULT ro = f_open(&fl, "XC360/BETAKEY.TXT", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK(ro != FR_OK, "abrir pra gravar falha (%d)", (int)ro);
    g_fail_writes = 0;
    DIR d;
    CHECK(f_opendir(&d, "/") == FR_OK, "o volume continua legível");
    f_mount(NULL, "", 0);

    printf("fatfs: %d verificações, %d falhas\n", g_checks, g_bad);
    return g_bad ? 1 : 0;
}
