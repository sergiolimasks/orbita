/*
 * orbita v0.21.0 — Wi-Fi interno: tira o 88W8786U do EHCI (tinyehci) e
 * entrega a porta pro OHCI companheiro.
 *
 * O tinyehci do libxenon fica com toda porta high-speed no boot (EHCI_Init ->
 * ehci_discover) e só sabe falar mass storage, síncrono. O adaptador Wi-Fi é
 * USB 2.0, então cai lá. Aqui: marcamos o dispositivo como "ocupado" (o
 * USBStorage_Init do usb_do_poll pula portas busy) e ligamos PORT_OWNER: a
 * porta passa pro OHCI, o dispositivo reenumera em full speed (12 Mbit/s) e
 * a pilha CFE do libxenon (assíncrona, a mesma dos controles) o enxerga sem
 * driver. Só a thread de rede (hw 1), dentro do mesmo passo do usb_do_poll.
 */
#include <stddef.h>
#include <stdint.h>
#include <xetypes.h>
#include <byteswap.h>
#include <pci/io.h>
#include <usb/tinyehci/ehci.h>

#ifndef EHCI_HCD_COUNT
#define EHCI_HCD_COUNT 2
#endif
extern struct ehci_hcd ehci_hcds[EHCI_HCD_COUNT];

int xw_ehci_release_wifi(int *bus, int *port, int release);

/* release = 0: só procura (não mexe em nada). 1 = acha e entrega ao OHCI. */
int xw_ehci_release_wifi(int *bus, int *port, int release) {
    for (int b = 0; b < EHCI_HCD_COUNT; b++) {
        struct ehci_hcd *h = &ehci_hcds[b];
        if (!h->regs) continue;
        int np = h->num_port;
        if (np > EHCI_MAX_ROOT_PORTS) np = EHCI_MAX_ROOT_PORTS;
        for (int p = 0; p < np; p++) {
            struct ehci_device *d = &h->devices[p];
            if (!d->id) continue;
            if (bswap_16(d->desc.idVendor) != 0x045e || bswap_16(d->desc.idProduct) != 0x0765) continue;
            *bus = b;
            *port = p + 1;
            if (!release) return 1;
            d->busy = 1;   /* USBStorage_Init: não reseta nem testa esta porta */
            d->id = 0;
            u32 reg = (u32)h->regs + (u32)offsetof(struct ehci_regs, port_status) + 4u * (u32)p;
            u32 st = read32(reg);
            st |= PORT_OWNER;
            st &= ~PORT_RWC_BITS;
            write32(reg, st);
            *bus = b;
            *port = p + 1;
            return 1;
        }
    }
    return 0;
}

/* v0.21.1: PORTSC da porta (1..n) do EHCI bus, pro log. */
uint32_t xw_ehci_port_status(int bus, int port);
uint32_t xw_ehci_port_status(int bus, int port) {
    if (bus < 0 || bus >= EHCI_HCD_COUNT || port < 1) return 0xffffffffu;
    struct ehci_hcd *h = &ehci_hcds[bus];
    if (!h->regs) return 0xffffffffu;
    return read32((u32)h->regs + (u32)offsetof(struct ehci_regs, port_status) + 4u * (u32)(port - 1));
}
