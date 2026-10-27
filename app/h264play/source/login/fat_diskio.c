/*
 * XCloud360 v0.17 — glue FatFs <-> pendrive USB do libxenon (tinyehci,
 * usb2mass_ops_N, DISC_INTERFACE). Um volume só: o primeiro dispositivo de
 * massa com FAT (o pendrive XELL de onde o xenon.elf bootou).
 *
 * USB não é thread-safe: quem chama isto (xc_port_xenon.c) segura o USB
 * antes (xc_usb_hold, a thread de rede para de chamar usb_do_poll).
 */
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

#include <string.h>
#include <diskio/disc_io.h>

extern DISC_INTERFACE usb2mass_ops_0, usb2mass_ops_1, usb2mass_ops_2;

static DISC_INTERFACE *g_disc = NULL;
/* Buffer de passagem alinhado (o EHCI faz DMA). 8 setores por vez. */
static uint8_t g_bounce[8 * 512] __attribute__((aligned(128)));

void fat_diskio_select(int dev) {
    static DISC_INTERFACE *ops[3] = { &usb2mass_ops_0, &usb2mass_ops_1, &usb2mass_ops_2 };
    g_disc = (dev >= 0 && dev < 3) ? ops[dev] : NULL;
}

int fat_diskio_sectors(int dev) {
    fat_diskio_select(dev);
    return g_disc ? (int)g_disc->sectors() : 0;
}

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return g_disc ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    if (!g_disc || g_disc->sectors() <= 0) return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!g_disc) return RES_NOTRDY;
    while (count) {
        UINT n = count > 8 ? 8 : count;
        if (!g_disc->readSectors((sec_t)sector, n, g_bounce)) return RES_ERROR;
        memcpy(buff, g_bounce, n * 512);
        buff += n * 512;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!g_disc) return RES_NOTRDY;
    while (count) {
        UINT n = count > 8 ? 8 : count;
        memcpy(g_bounce, buff, n * 512);
        if (!g_disc->writeSectors((sec_t)sector, n, g_bounce)) return RES_ERROR;
        buff += n * 512;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    if (!g_disc) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;   /* escrita é síncrona no tinyehci */
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)g_disc->sectors(); return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

/* v0.21.7: um setor direto no disco, com buffer próprio (não o g_bounce, que
 * um FatFs parado no meio pode estar usando). Só o vigia (xc_hang_write). */
int fat_diskio_raw_write(uint32_t sector, const void *buf512) {
    static uint8_t b[512] __attribute__((aligned(128)));
    if (!g_disc) return -1;
    memcpy(b, buf512, 512);
    return g_disc->writeSectors((sec_t)sector, 1, b) ? 0 : -1;
}
