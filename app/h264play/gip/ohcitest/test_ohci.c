/*
 * Host test of the REAL libxenon ohci.c interrupt-tree code (patched with
 * usb_open_pipe_interval placement): pools, tree, ept_create, xfer,
 * ept_delete.  Endpoint set from the v3 hardware log: bus 1 with the RF
 * module 045e:02aa (4 x IN/OUT interrupt EDs, stock 32 ms placement) and
 * the Series pad 045e:0b12 (OUT 0x02 + IN 0x82, interval 4).
 * The HC is emulated by walking the HCCA interrupt table exactly as the
 * hardware does (hcca_inttable[frame % 32] -> ed_next_ed ... -> 0).
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include "cfe.h"

/* ---- arena mapped at 0x10000000 and its uncached alias (phys|0xA0000000, as on Xenon) */
#define ARENA_LO 0x10000000UL
#define ARENA_SZ (8u << 20)
static uint8_t *arena;
static size_t arena_used;
static long live;
void *mock_kmalloc(size_t n, int align)
{
    size_t a = align < 8 ? 8 : (size_t)align;
    arena_used = (arena_used + a - 1) & ~(a - 1);
    if (arena_used + n > ARENA_SZ) { fprintf(stderr, "arena full\n"); abort(); }
    void *p = arena + arena_used;
    arena_used += n ? n : 8;
    memset(p, 0, n);
    live++;
    return p;
}
void mock_kfree(void *p) { if (p) live--; }

uint64_t mock_now_ms = 1000;
static uint32_t frame;
uint32_t read32(uint64_t addr) { (void)addr; return frame++; }   /* FMNUMBER advances */
void write32(uint64_t addr, uint32_t v) { (void)addr; (void)v; }
void memdcbst(void *a, int l) { (void)a; (void)l; }
void memdcbf(void *a, int l) { (void)a; (void)l; }
static int n_complete, last_status;
struct usbreq_s;
void usb_complete_request(struct usbreq_s *ur, int status);

#include "ohci.c"     /* the real thing, statics included */

void usb_delay_ms(usbbus_t *bus, int ms) { (void)bus; (void)ms; }

void usb_complete_request(usbreq_t *ur, int status)
{
    ur->ur_status = status;
    ur->ur_inprogress = 0;
    n_complete++;
    last_status = status;
}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static ohci_softc_t *softc;
static usbbus_t bus;

/* emulate the HC's periodic walk from every HCCA slot; count visits of ed_phys */
static int visits(uint32_t ed_phys, int *cycle)
{
    int slot, n = 0;
    *cycle = 0;
    for (slot = 0; slot < OHCI_INTTABLE_SIZE; slot++) {
        uint32_t p = BSWAP32(softc->ohci_hcca->hcca_inttable[slot]);
        int steps = 0;
        while (p) {
            if (p == ed_phys) n++;
            ohci_ed_t *ed = (ohci_ed_t *)OHCI_PTOV(p);
            p = BSWAP32(ed->ed_next_ed) & M_OHCI_ED_PTRMASK;
            if (++steps > 400) { *cycle = 1; break; }
        }
    }
    return n;
}

/* software list (ep_next) must contain e when walked from inttable[0] */
static int on_soft_list(ohci_endpoint_t *e)
{
    ohci_endpoint_t *c = softc->ohci_inttable[0];
    int steps = 0;
    while (c && steps++ < 400) { if (c == e) return 1; c = c->ep_next; }
    return 0;
}

static int freelist_len(void)
{
    int n = 0;
    ohci_endpoint_t *e = softc->ohci_endpoint_freelist;
    while (e && n < 1000) { n++; e = e->ep_next; }
    return n;
}

static ohci_endpoint_t *mk(int addr, int ep, int mps, int flags)
{
    return (ohci_endpoint_t *)ohci_ept_create(&bus, addr, ep, mps, flags);
}

int main(void)
{
    int fd = memfd_create("arena", 0), cyc;
    if (fd < 0 || ftruncate(fd, ARENA_SZ)) { perror("memfd"); return 2; }
    arena = mmap((void *)ARENA_LO, ARENA_SZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    void *alias = mmap((void *)(ARENA_LO | 0xA0000000UL), ARENA_SZ, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, 0);
    if (arena == MAP_FAILED || alias == MAP_FAILED) { perror("mmap"); return 2; }

    softc = KMALLOC(sizeof(*softc), 0);
    bus.ub_hwsoftc = (usb_hc_t *)softc;
    softc->ohci_regs = 0;
    softc->ohci_rh_addr = 1;
    CHECK(_ohci_initpools(softc) == 0);
    CHECK(_ohci_setupepts(softc) == 0);
    int free0 = freelist_len();

    /* tree sanity: every HCCA slot walks to the end without a cycle */
    visits(0xdeadbeef, &cyc);
    CHECK(!cyc);
    CHECK(softc->ohci_inttable[0] == softc->ohci_edtable[31]);

    /* RF module 02aa on addr 2: 4 x (IN 0x81+2k, OUT 0x01+2k) mps 32, stock placement */
    ohci_endpoint_t *rf[8];
    for (int k = 0; k < 4; k++) {
        rf[2 * k] = mk(2, 1 + 2 * k, 32, UP_TYPE_INTR | UP_TYPE_IN);
        rf[2 * k + 1] = mk(2, 1 + 2 * k, 32, UP_TYPE_INTR | UP_TYPE_OUT);
    }
    for (int k = 0; k < 8; k++) {
        CHECK(visits(rf[k]->ep_phys, &cyc) == 1 && !cyc);     /* every 32 ms */
        CHECK(on_soft_list(rf[k]));
    }

    /* Series pad 0b12 on addr 3: OUT 0x02 then IN 0x82, mps 64, interval 4 */
    ohci_endpoint_t *gout = mk(3, 2, 64, UP_TYPE_INTR | UP_TYPE_OUT | (4 << UP_IVAL_SHIFT));
    ohci_endpoint_t *gin = mk(3, 2, 64, UP_TYPE_INTR | UP_TYPE_IN | (4 << UP_IVAL_SHIFT));
    CHECK(gout && gin);
    CHECK(visits(gout->ep_phys, &cyc) == 8 && !cyc);           /* every 4 ms */
    CHECK(visits(gin->ep_phys, &cyc) == 8 && !cyc);
    CHECK(on_soft_list(gout) && on_soft_list(gin));
    for (int k = 0; k < 8; k++) CHECK(visits(rf[k]->ep_phys, &cyc) == 1 && !cyc);
    /* ED control word: FA 3, EN 2, MPS 64, direction */
    ohci_ed_t *ed = ohci_ed_from_endpoint(softc, gin);
    uint32_t ctl = BSWAP32(ed->ed_control);
    CHECK(G_OHCI_ED_FA(ctl) == 3 && G_OHCI_ED_EN(ctl) == 2 && G_OHCI_ED_MPS(ctl) == 64);
    CHECK(G_OHCI_ED_DIR(ctl) == K_OHCI_ED_DIR_IN);
    CHECK(!(ctl & M_OHCI_ED_SKIP));

    /* other intervals land at the right depth */
    static const int iv[] = { 1, 2, 3, 8, 16, 32, 255 }, want[] = { 32, 16, 16, 4, 2, 1, 1 };
    for (unsigned i = 0; i < sizeof(iv) / sizeof(iv[0]); i++) {
        ohci_endpoint_t *e = mk(4, 1, 8, UP_TYPE_INTR | UP_TYPE_IN | (iv[i] << UP_IVAL_SHIFT));
        CHECK(visits(e->ep_phys, &cyc) == want[i] && !cyc);
        CHECK(on_soft_list(e));
        ohci_ept_delete(&bus, (usb_ept_t *)e);
        CHECK(visits(e->ep_phys, &cyc) == 0 && !cyc);
    }

    /* queue an IN transfer (64 bytes, short OK) on the 4 ms ED, as gip_start_read does */
    static usbdev_t dev;
    static usbpipe_t pipe;
    static usbreq_t ur;
    uint8_t *buf = KMALLOC(128, 128);
    dev.ud_address = 3;
    dev.ud_bus = &bus;
    pipe.up_hwendpoint = (usb_ept_t *)gin;
    pipe.up_dev = &dev;
    pipe.up_mps = 64;
    ur.ur_dev = &dev; ur.ur_pipe = &pipe; ur.ur_buffer = buf; ur.ur_length = 64;
    ur.ur_flags = UR_FLAG_IN | UR_FLAG_SHORTOK; ur.ur_inprogress = 1;
    CHECK(ohci_xfer(&bus, (usb_ept_t *)gin, &ur) == 0);
    {   /* v5 dump helper on the real structures: the 4 ms IN ED is reachable from 8 slots */
        char l[220];
        dev.ud_pipes[USB_EPADDR_TO_IDX(0x82)] = &pipe;
        ohci_debug_ept(&dev, 0x82, l, sizeof(l));
        CHECK(strstr(l, " vis8 ") != NULL && strstr(l, "ep82") != NULL && strstr(l, "td c=00000000") == NULL);
        CHECK(ohci_debug_hc(&bus, l, sizeof(l)) > 0 && strstr(l, "hc0 ctl=") != NULL);
        dev.ud_pipes[USB_EPADDR_TO_IDX(0x82)] = NULL;
    }
    CHECK((BSWAP32(ed->ed_headp) & M_OHCI_ED_PTRMASK) != (BSWAP32(ed->ed_tailp) & M_OHCI_ED_PTRMASK));
    /* the TD buffer points at our DMA buffer */
    ohci_td_t *td = (ohci_td_t *)OHCI_PTOV(BSWAP32(ed->ed_headp) & M_OHCI_ED_PTRMASK);
    CHECK(BSWAP32(td->td_cbp) == OHCI_VTOD(buf) && BSWAP32(td->td_be) == OHCI_VTOD(buf + 63));

    /* unplug: delete OUT and IN (IN has a pending TD -> completed as cancelled) */
    n_complete = 0;
    ohci_ept_delete(&bus, (usb_ept_t *)gout);
    ohci_ept_delete(&bus, (usb_ept_t *)gin);
    CHECK(n_complete == 1 && last_status == K_OHCI_CC_CANCELLED);
    CHECK(visits(gout->ep_phys, &cyc) == 0 && visits(gin->ep_phys, &cyc) == 0 && !cyc);
    CHECK(!on_soft_list(gout) && !on_soft_list(gin));
    for (int k = 0; k < 8; k++) CHECK(visits(rf[k]->ep_phys, &cyc) == 1 && !cyc);

    /* done queue with the HccaDoneHead flag bit (bit 0) set: must still
     * complete OUR request with the right length (v4 fix: mask the pointer) */
    {
        ohci_endpoint_t *e = mk(3, 2, 64, UP_TYPE_INTR | UP_TYPE_IN | (4 << UP_IVAL_SHIFT));
        ohci_ed_t *ed2 = ohci_ed_from_endpoint(softc, e);
        static usbreq_t ur2;
        memset(&ur2, 0, sizeof(ur2));
        ur2.ur_dev = &dev; ur2.ur_pipe = &pipe; ur2.ur_buffer = buf; ur2.ur_length = 64;
        ur2.ur_flags = UR_FLAG_IN | UR_FLAG_SHORTOK; ur2.ur_inprogress = 1;
        pipe.up_hwendpoint = (usb_ept_t *)e;
        CHECK(ohci_xfer(&bus, (usb_ept_t *)e, &ur2) == 0);
        ohci_td_t *t2 = (ohci_td_t *)OHCI_PTOV(BSWAP32(ed2->ed_headp) & M_OHCI_ED_PTRMASK);
        /* emulate the HC retiring it: CC=0, 32-byte short packet, off the ED */
        uint32_t c2 = BSWAP32(t2->td_control);
        c2 &= ~V_OHCI_TD_CC(0xf);
        t2->td_control = BSWAP32(c2);
        t2->td_cbp = BSWAP32(OHCI_VTOD(buf) + 32);
        ed2->ed_headp = ed2->ed_tailp;
        t2->td_next_td = 0;
        uint32_t phys = OHCI_VTOP(t2);
        n_complete = 0;
        ohci_intr_procdoneq(softc, BSWAP32(phys | 1));
        CHECK(n_complete == 1 && last_status == 0 && ur2.ur_xferred == 32);
        ohci_ept_delete(&bus, (usb_ept_t *)e);
    }

    /* exhausted TD pool: the transfer is refused cleanly, no NULL-TD write */
    {
        ohci_endpoint_t *e = mk(3, 2, 64, UP_TYPE_INTR | UP_TYPE_IN | (4 << UP_IVAL_SHIFT));
        ohci_transfer_t *saved = softc->ohci_transfer_freelist;
        softc->ohci_transfer_freelist = NULL;            /* pool empty */
        static usbreq_t ur3;
        memset(&ur3, 0, sizeof(ur3));
        ur3.ur_dev = &dev; ur3.ur_pipe = &pipe; ur3.ur_buffer = buf; ur3.ur_length = 64;
        ur3.ur_flags = UR_FLAG_IN; ur3.ur_inprogress = 1;
        CHECK(ohci_xfer(&bus, (usb_ept_t *)e, &ur3) == -1);
        CHECK(ur3.ur_inprogress == 0 && ur3.ur_status != 0);
        CHECK(mk(3, 3, 64, UP_TYPE_INTR | UP_TYPE_IN) == NULL);   /* no ED without a tail TD */
        softc->ohci_transfer_freelist = saved;
        CHECK(ohci_td_low_water >= 0 && ohci_td_low_water < OHCI_TDPOOL_SIZE);
        ohci_ept_delete(&bus, (usb_ept_t *)e);
    }

    /* replug storm: 500 x create/delete at 4 ms, interleaved with RF churn */
    for (int i = 0; i < 500; i++) {
        ohci_endpoint_t *o = mk(3, 2, 64, UP_TYPE_INTR | UP_TYPE_OUT | (4 << UP_IVAL_SHIFT));
        ohci_endpoint_t *n = mk(3, 2, 64, UP_TYPE_INTR | UP_TYPE_IN | (4 << UP_IVAL_SHIFT));
        if (!o || !n) { CHECK(0); break; }
        if (i % 7 == 0) { ohci_ept_delete(&bus, (usb_ept_t *)rf[i % 8]);
                          rf[i % 8] = mk(2, 1 + (i % 8), 32, UP_TYPE_INTR | UP_TYPE_IN); }
        if (visits(n->ep_phys, &cyc) != 8 || cyc) { CHECK(0); break; }
        ohci_ept_delete(&bus, (usb_ept_t *)n);
        ohci_ept_delete(&bus, (usb_ept_t *)o);
    }
    for (int k = 0; k < 8; k++) CHECK(visits(rf[k]->ep_phys, &cyc) == 1 && !cyc);
    for (int k = 0; k < 8; k++) ohci_ept_delete(&bus, (usb_ept_t *)rf[k]);
    CHECK(freelist_len() == free0);                  /* no ED leaked */
    visits(0, &cyc);
    CHECK(!cyc);

    printf("ohci model: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
