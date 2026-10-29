/*
 * orbita v0.21.8 — shims pra compilar source/wifi/xw_ehs_xenon.c no host
 * (hosttest/wifi/stress). Memória de DMA numa arena com endereços de 32 bits,
 * sem cache (UNC = identidade), little-endian no host (LE = identidade: no
 * Xbox o LE() troca bytes porque a CPU é big-endian e o EHCI é LE; aqui os
 * dois lados são o mesmo programa x86, então o modelo do controlador lê o
 * valor nativo). Registradores = campos de structs comuns.
 */
#ifndef XHS_HOST_H
#define XHS_HOST_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

#define bswap_16(x) __builtin_bswap16(x)
#define PPC_TIMEBASE_FREQ 1000000ULL          /* mftb em microssegundos */
uint64_t mftb(void);
static inline unsigned long tb_diff_msec(uint64_t a, uint64_t b) { return (unsigned long)((a - b) / 1000); }
void udelay(int us);
static inline void memdcbf(void *a, int n) { (void)a; (void)n; }
static inline void memdcbst(void *a, int n) { (void)a; (void)n; }
static inline uint32_t read32(uintptr_t a) { return *(volatile uint32_t *)a; }
void write32(uintptr_t a, uint32_t v);
void xc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void    *xhs_host_memalign(size_t align, size_t n);
uint32_t xhs_host_dma(const volatile void *p);
void    *xhs_host_ptr(uint32_t dma);
#define memalign xhs_host_memalign
#define UNC(p)   ((void *)(p))
#define DMA(p)   xhs_host_dma((const volatile void *)(p))
#define REG(base, off) ((uintptr_t)(base) + (uintptr_t)(off))
#define LE(v)    ((uint32_t)(v))

struct ehci_caps { uint32_t hc_capbase, hcs_params, hcc_params; };
struct ehci_regs {
    uint32_t command, status, intr_enable, frame_index, segment, frame_list, async_next;
    uint32_t reserved[9];
    uint32_t configured_flag;
    uint32_t port_status[8];
};
struct ehci_qh {
    uint32_t hw_next, hw_info1, hw_info2, hw_current, hw_qtd_next, hw_alt_next, hw_token, hw_buf[5], hw_buf_hi[5];
    uint32_t qh_dma;
};
typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
} usb_devdesc;
struct ehci_device { usb_devdesc desc; int id, port, fd; uint32_t toggles; int busy; };
#define EHCI_MAX_ROOT_PORTS 8
struct ehci_hcd {
    void *_regs;
    struct ehci_caps *caps;
    struct ehci_regs *regs;
    struct ehci_qh *async;
    int num_port;
    struct ehci_device devices[EHCI_MAX_ROOT_PORTS];
};
#endif
