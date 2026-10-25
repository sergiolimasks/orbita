/*
 * Host test for the libxenon GIP driver.
 *   - gip_proto.c: init builder, ack/rumble builder, report 0x20/0x07 parser
 *   - usbgip.c + usbdevs.c + input.c against a mock USB core: matching,
 *     endpoint discovery, init chaining + sequence numbers, guide ack,
 *     re-init on announce, unplug/replug, slot sharing, leak checks.
 * Build: see run.sh.
 */
#include "cfe.h"
#include "usbchap9.h"
#include "usbd.h"
#include "gip_proto.h"
#include <stdarg.h>
#include <assert.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------ mock heap */
static long live_allocs = 0, total_allocs = 0;
void *mock_kmalloc(size_t n, int align) { (void)align; live_allocs++; total_allocs++; return calloc(1, n ? n : 1); }
void mock_kfree(void *p) { if (p) { live_allocs--; free(p); } }

/* ------------------------------------------------------------ log capture */
#include "usblog.h"
uint64_t mock_now_ms = 1000;
int ohci_td_low_water = 60;     /* ohci.c symbols used by usbgip_summary */
unsigned ohci_td_refused;
usbdev_t *RFdev;
unsigned usbctrl_rx_count;
static int n_dump_calls;
int ohci_debug_ept(usbdev_t *dev, int epaddr, char *out, int cap)
{ n_dump_calls++; return snprintf(out, cap, "mock ed a%d ep%02x", dev ? dev->ud_address : -1, epaddr); }
int ohci_debug_hc(usbbus_t *bus, char *out, int cap) { (void)bus; return snprintf(out, cap, "mock hc"); }
extern int usbgip_force_variant;
/* Model of the app's login/net_pipe.c log pipe: 64-line ring, dropped when
 * full, consumer (log_step, 8 lines) runs on the SAME thread between polls,
 * nothing before np_init. */
#define NP_LINES 64
static char np_ring[NP_LINES][220];
static unsigned np_w, np_r, np_drops;
static int np_inited = 0;
static int n_np = 0;                 /* np_log_line calls */
static int np_calls_this_tick = 0;
static unsigned np_delivered;
static char np_last[256];
static int np_saw_boot0 = 0, np_saw_sum = 0, np_saw_enum = 0;
void np_log_line(const char *t)
{
    n_np++;
    np_calls_this_tick++;
    if (!np_inited) return;
    if (np_w - np_r >= NP_LINES) { np_drops++; return; }
    snprintf(np_ring[np_w % NP_LINES], 220, "LOG %s", t);
    np_w++;
}
static void np_consume(int k)
{
    while (k-- > 0 && np_r != np_w) {
        const char *l = np_ring[np_r % NP_LINES];
        snprintf(np_last, sizeof(np_last), "%s", l);
        if (strstr(l, "[gip] sum#")) np_saw_sum++;
        if (strstr(l, "enum bus")) np_saw_enum++;
        if (strstr(l, "] t=0.000 ")) np_saw_boot0++;
        if (getenv("GIP_VERBOSE")) printf("  UDP %s\n", l);
        np_r++;
        np_delivered++;
    }
}
static int log_has(const char *s) { return usblog_find(s, 40); }
extern void usbgip_tick(void);

/* ------------------------------------------------------------ mock USB core */
usb_driver_t usbhub_driver = { "hub", 0, 0 }, usbhid_driver = { "hid", 0, 0 },
             usbctrl_driver = { "360", 0, 0 }, dummy_driver = { "dummy", 0, 0 };
extern usb_driver_t usbgip_driver;

static int slot_mask = 0;
int usbctrl_slot_alloc(void) { int i; for (i = 0; i < 4 && (slot_mask & (1 << i)); ++i); if (i >= 4) return -1; slot_mask |= 1 << i; return i; }
void usbctrl_slot_free(int i) { if (i >= 0 && i < 4) slot_mask &= ~(1 << i); }
int usbctrl_set_rumble(int port, uint8_t l, uint8_t r) { (void)port; (void)l; (void)r; return -1; }
extern int usbgip_set_rumble(int port, uint8_t l, uint8_t r);

static int n_setconfig, n_setif, n_pipes, n_setif_any, last_setif_alt = -1, last_setif_if = -1;
static usbreq_t *inflight_in, *inflight_out, *inflight_in2;
static uint8_t out_log[64][GIP_MAX_PKT]; static int out_len[64], n_out;

int usb_set_configuration(usbdev_t *dev, int config) { (void)dev; (void)config; n_setconfig++; return 0; }
int usb_simple_request(usbdev_t *dev, uint8_t rt, int req, int val, int idx)
{
    (void)dev;
    if (rt == 0x01 && req == USB_REQUEST_SET_INTERFACE) {
        n_setif_any++; last_setif_alt = val; last_setif_if = idx;
        if (val == 0 && idx == 1) n_setif++;
    }
    return 0;
}
int usb_open_pipe(usbdev_t *dev, usb_endpoint_descr_t *ep)
{
    int i = USB_EPADDR_TO_IDX(ep->bEndpointAddress);
    dev->ud_pipes[i] = (usbpipe_t *)KMALLOC(sizeof(usbpipe_t), 0);
    dev->ud_pipes[i]->up_mps = GETUSBFIELD(ep, wMaxPacketSize);
    n_pipes++;
    return ep->bEndpointAddress;
}
static int last_ival[2];
int usb_open_pipe_interval(usbdev_t *dev, usb_endpoint_descr_t *ep, int ival)
{
    last_ival[USB_ENDPOINT_DIR_IN(ep->bEndpointAddress) ? 1 : 0] = ival;
    return usb_open_pipe(dev, ep);
}
usbreq_t *usb_make_request(usbdev_t *dev, int ep, uint8_t *buf, int len, int flags)
{
    usbpipe_t *p = dev->ud_pipes[USB_EPADDR_TO_IDX(ep)];
    if (!p) return NULL;
    usbreq_t *ur = KMALLOC(sizeof(usbreq_t), 0);
    ur->ur_dev = dev; ur->ur_pipe = p; ur->ur_buffer = buf; ur->ur_length = len; ur->ur_flags = flags;
    return ur;
}
int usb_queue_request(usbreq_t *ur)
{
    assert(!ur->ur_inprogress);
    ur->ur_inprogress = 1; ur->ur_xferred = 0;
    if (ur->ur_flags & UR_FLAG_IN) {
        if (!inflight_in) inflight_in = ur;
        else { assert(!inflight_in2); inflight_in2 = ur; }
    }
    else {
        assert(!inflight_out); inflight_out = ur;
        if (n_out < 64) { memcpy(out_log[n_out], ur->ur_buffer, ur->ur_length); out_len[n_out] = ur->ur_length; }
        n_out++;
    }
    return 0;
}
void usb_free_request(usbreq_t *ur) { assert(!ur->ur_inprogress); KFREE(ur); }
void *usb_find_cfg_descr(usbdev_t *dev, int dtype, int idx)
{
    uint8_t *p = (uint8_t *)dev->ud_cfgdescr, *end;
    if (!p) return NULL;
    end = p + GETUSBFIELD(dev->ud_cfgdescr, wTotalLength);
    while (p < end) { if (p[1] == dtype) { if (idx == 0) return p; idx--; } p += p[0]; }
    return NULL;
}
static void complete(usbreq_t **slot, int status, const uint8_t *data, int len)
{
    usbreq_t *ur = *slot;
    assert(ur);
    *slot = NULL;
    if (data && len) memcpy(ur->ur_buffer, data, len);
    ur->ur_xferred = len; ur->ur_status = status; ur->ur_inprogress = 0;
    ur->ur_callback(ur);
}
static void complete_out_ok(void) { complete(&inflight_out, 0, NULL, inflight_out->ur_length); }
static void in_pkt(const uint8_t *d, int n) { complete(&inflight_in, 0, d, n); }
static int drain_out(void) { int k = 0; while (inflight_out) { complete_out_ok(); k++; } return k; }

/* hub-style removal: flag, destroy pipes (cancels in-flight), detach */
static void unplug(usbdev_t *dev)
{
    dev->ud_flags |= UD_FLAG_REMOVING;
    if (inflight_in) complete(&inflight_in, UR_ERR_CANCELLED, NULL, 0);
    if (inflight_in2) complete(&inflight_in2, UR_ERR_CANCELLED, NULL, 0);
    if (inflight_out) complete(&inflight_out, UR_ERR_CANCELLED, NULL, 0);
    for (int i = 0; i < UD_MAX_PIPES; i++) if (dev->ud_pipes[i]) { KFREE(dev->ud_pipes[i]); dev->ud_pipes[i] = NULL; }
    dev->ud_drv->udrv_detach(dev);
}

/* ------------------------------------------------------------ descriptors */
/* Synthetic, shaped like an Xbox One S (lsusb -v): 3 interfaces, all
 * ff/47/d0; if0 = interrupt OUT 0x02 then IN 0x82 (OUT listed first on
 * purpose), if1 alt1 = isoc audio, if2 alt1 = bulk. */
static const uint8_t cfg_one_s[] = {
    9, 2, 0, 0, 3, 1, 0, 0xa0, 0xfa,
    9, 4, 0, 0, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x02, 3, 64, 0, 4,
      7, 5, 0x82, 3, 64, 0, 4,
    9, 4, 1, 0, 0, 0xff, 0x47, 0xd0, 0,
    9, 4, 1, 1, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x03, 1, 228, 0, 1,
      7, 5, 0x83, 1, 64, 0, 1,
    9, 4, 2, 0, 0, 0xff, 0x47, 0xd0, 0,
    9, 4, 2, 1, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x04, 2, 64, 0, 0,
      7, 5, 0x84, 2, 64, 0, 0,
};
/* 360 wired pad: vendor class, subclass 93 */
static const uint8_t cfg_360[] = {
    9, 2, 0, 0, 1, 1, 0, 0xa0, 0xfa,
    9, 4, 0, 0, 2, 0xff, 0x5d, 0x01, 0,
      7, 5, 0x81, 3, 32, 0, 4,
      7, 5, 0x01, 3, 32, 0, 8,
};
static const uint8_t cfg_other[] = {
    9, 2, 0, 0, 1, 1, 0, 0xa0, 0xfa,
    9, 4, 0, 0, 1, 0xff, 0x00, 0x00, 0,
      7, 5, 0x81, 3, 8, 0, 4,
};

static usbdev_t *mkdev(uint16_t vid, uint16_t pid, uint8_t dclass, uint8_t dsub, uint8_t dproto,
                       const uint8_t *cfg, int cfglen)
{
    usbdev_t *d = calloc(1, sizeof(*d));
    static usbbus_t bus;
    d->ud_bus = &bus;
    d->ud_address = 1;
    PUTUSBFIELD(&d->ud_devdescr, idVendor, vid);
    PUTUSBFIELD(&d->ud_devdescr, idProduct, pid);
    d->ud_devdescr.bDeviceClass = dclass;
    d->ud_devdescr.bDeviceSubClass = dsub;
    d->ud_devdescr.bDeviceProtocol = dproto;
    d->ud_devdescr.bNumConfigurations = 1;
    uint8_t *c = malloc(cfglen);
    memcpy(c, cfg, cfglen);
    c[2] = cfglen & 0xff; c[3] = cfglen >> 8;
    d->ud_cfgdescr = (usb_config_descr_t *)c;
    return d;
}
static void freedev(usbdev_t *d) { free(d->ud_cfgdescr); free(d); }

/* ------------------------------------------------------------ packet helpers */
static int mk_input(uint8_t *b, uint8_t seq, uint8_t btn0, uint8_t btn1, uint16_t lt, uint16_t rt,
                    int16_t lx, int16_t ly, int16_t rx, int16_t ry, int total)
{
    memset(b, 0, GIP_MAX_PKT);
    b[0] = 0x20; b[1] = 0x00; b[2] = seq; b[3] = (uint8_t)(total - 4);
    b[4] = btn0; b[5] = btn1;
    b[6] = lt & 0xff; b[7] = lt >> 8; b[8] = rt & 0xff; b[9] = rt >> 8;
    b[10] = (uint16_t)lx & 0xff; b[11] = (uint16_t)lx >> 8;
    b[12] = (uint16_t)ly & 0xff; b[13] = (uint16_t)ly >> 8;
    b[14] = (uint16_t)rx & 0xff; b[15] = (uint16_t)rx >> 8;
    b[16] = (uint16_t)ry & 0xff; b[17] = (uint16_t)ry >> 8;
    return total;
}

/* ============================================================ proto tests */
static void test_init_builder(void)
{
    gip_pkt_t p[GIP_MAX_INIT];
    static const uint8_t power_on[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
    static const uint8_t s_init[] = { 0x05, 0x20, 0x00, 0x0f, 0x06 };
    static const uint8_t led_on[] = { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
    static const uint8_t auth[] = { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 };
    static const uint8_t extra[] = { 0x4d, 0x10, 0x01, 0x02, 0x07, 0x00 };
    int n;

    n = gip_build_init(0x045e, 0x02d1, p, GIP_MAX_INIT);              /* One */
    CHECK(n == 3);
    CHECK(p[0].len == 5 && !memcmp(p[0].data, power_on, 5));
    CHECK(p[1].len == 7 && !memcmp(p[1].data, led_on, 7));
    CHECK(p[2].len == 6 && !memcmp(p[2].data, auth, 6));
    CHECK(!(p[0].flags & GIP_PF_INIT_LAST) && (p[2].flags & GIP_PF_INIT_LAST));
    CHECK(p[0].flags & GIP_PF_STAMP_SEQ);

    n = gip_build_init(0x045e, 0x02ea, p, GIP_MAX_INIT);              /* One S */
    CHECK(n == 4 && !memcmp(p[0].data, power_on, 5) && !memcmp(p[1].data, s_init, 5));
    n = gip_build_init(0x045e, 0x0b00, p, GIP_MAX_INIT);              /* Elite 2 */
    CHECK(n == 5 && !memcmp(p[1].data, s_init, 5) && !memcmp(p[2].data, extra, 6) &&
          !memcmp(p[3].data, led_on, 7));
    n = gip_build_init(0x045e, 0x0b12, p, GIP_MAX_INIT);              /* Series */
    CHECK(n == 3);
    n = gip_build_init(0x24c6, 0x543a, p, GIP_MAX_INIT);              /* PowerA */
    CHECK(n == 5 && p[3].data[0] == 0x09 && p[3].data[8] == 0x1d && p[4].data[8] == 0x00);
    n = gip_build_init(0x0e6f, 0x0165, p, GIP_MAX_INIT);              /* PDP Titanfall */
    CHECK(n == 4 && p[0].data[0] == 0x01 && p[0].len == 13 && p[0].data[5] == 0x04);
    n = gip_build_init(0x045e, 0x02ea, p, 2);                         /* bounded */
    CHECK(n == 2 && (p[1].flags & GIP_PF_INIT_LAST));
}

static void test_host_builders(void)
{
    gip_pkt_t p;
    static const uint8_t pw[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
    static const uint8_t id[] = { 0x04, 0x20, 0x00, 0x00 };
    static const uint8_t loc[] = { 0x05, 0x20, 0x00, 0x0f, 0x06, 0, 0, 0, 0, 0, 0, 'U', 'S', 0, 0, 0, 0, 0, 0 };
    gip_build_power_on(&p, 1); CHECK(p.len == 5 && !memcmp(p.data, pw, 5) && !(p.flags & GIP_PF_STAMP_SEQ));
    gip_build_power_on(&p, 0); CHECK(p.flags & GIP_PF_STAMP_SEQ);
    gip_build_identify(&p); CHECK(p.len == 4 && !memcmp(p.data, id, 4) && (p.flags & GIP_PF_STAMP_SEQ));
    gip_build_locale(&p); CHECK(p.len == 19 && !memcmp(p.data, loc, 19));
    gip_build_led_on(&p); CHECK(p.len == 7 && p.data[0] == 0x0a && p.data[6] == 0x14);
}

static void test_seq(void)
{
    uint8_t c = 0;
    CHECK(gip_next_seq(&c) == 1 && gip_next_seq(&c) == 2);
    c = 255;
    CHECK(gip_next_seq(&c) == 255 && gip_next_seq(&c) == 1);   /* skips 0 */
}

static void test_ack_rumble(void)
{
    /* xpad mode_report_ack with seq 0x42 */
    static const uint8_t want[] = { 0x01, 0x20, 0x42, 0x09, 0x00, 0x07, 0x20, 0x02, 0, 0, 0, 0, 0 };
    gip_pkt_t a;
    gip_build_ack(&a, 0x42, 0x07, 2, 0);
    CHECK(a.len == 13 && !memcmp(a.data, want, 13) && !(a.flags & GIP_PF_STAMP_SEQ));
    gip_build_rumble(&a, 255, 128);
    CHECK(a.len == 13 && a.data[0] == 0x09 && a.data[5] == 0x0f && a.data[8] == 127 &&
          a.data[9] == 64 && a.data[10] == 0xff && a.data[12] == 0xff);
}

static void test_parse(void)
{
    gip_state_t st; gip_pkt_t ack; uint8_t b[GIP_MAX_PKT]; int ev, n;
    memset(&st, 0, sizeof(st));

    /* idle report as sent by a 02d1 (plen 0x0e -> 18 bytes) */
    n = mk_input(b, 1, 0, 0, 0, 0, 0, 0, 0, 0, 18);
    ev = gip_parse(&st, b, n, &ack);
    CHECK(ev == GIP_EV_INPUT && st.have_input);
    CHECK(!st.c.a && !st.c.start && st.c.lt == 0 && st.c.s1_x == 0);

    /* every button, max triggers, stick extremes */
    n = mk_input(b, 2, 0xfc, 0xff, 1023, 1023, -32768, 32767, 32767, -32768, 18);
    ev = gip_parse(&st, b, n, &ack);
    CHECK(ev == GIP_EV_INPUT);
    CHECK(st.c.start && st.c.back && st.c.a && st.c.b && st.c.x && st.c.y);
    CHECK(st.c.up && st.c.down && st.c.left && st.c.right && st.c.lb && st.c.rb);
    CHECK(st.c.s1_z && st.c.s2_z);
    CHECK(st.c.lt == 255 && st.c.rt == 255);
    CHECK(st.c.s1_x == -32768 && st.c.s1_y == 32767 && st.c.s2_x == 32767 && st.c.s2_y == -32768);
    CHECK(!st.c.logo);

    /* individual bits (xpad mapping) */
    n = mk_input(b, 3, 0x04, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack);
    CHECK(st.c.start && !st.c.back && !st.c.a);
    n = mk_input(b, 3, 0x08, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack);
    CHECK(!st.c.start && st.c.back);
    n = mk_input(b, 3, 0x10, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.a && !st.c.b);
    n = mk_input(b, 3, 0x20, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.b && !st.c.a);
    n = mk_input(b, 3, 0x40, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.x && !st.c.y);
    n = mk_input(b, 3, 0x80, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.y && !st.c.x);
    n = mk_input(b, 3, 0, 0x01, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.up && !st.c.down);
    n = mk_input(b, 3, 0, 0x02, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.down && !st.c.up);
    n = mk_input(b, 3, 0, 0x04, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.left && !st.c.right);
    n = mk_input(b, 3, 0, 0x08, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.right && !st.c.left);
    n = mk_input(b, 3, 0, 0x10, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.lb && !st.c.rb);
    n = mk_input(b, 3, 0, 0x20, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.rb && !st.c.lb);
    n = mk_input(b, 3, 0, 0x40, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.s1_z && !st.c.s2_z);
    n = mk_input(b, 3, 0, 0x80, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack); CHECK(st.c.s2_z && !st.c.s1_z);
    /* sync button (b4 bit0) maps to nothing */
    n = mk_input(b, 3, 0x01, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack);
    CHECK(!st.c.a && !st.c.start && !st.c.back);

    /* trigger scaling 10 bit -> 8 bit, and out-of-range clamp */
    n = mk_input(b, 4, 0, 0, 512, 4, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack);
    CHECK(st.c.lt == 128 && st.c.rt == 1);
    n = mk_input(b, 4, 0, 0, 0xffff, 3, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack);
    CHECK(st.c.lt == 255 && st.c.rt == 0);

    /* "stick up" = positive Y, like the raw 360 report usbctrl.c returns
     * (main.c menu_read_buttons: d.s1_y > 20000 == up) */
    n = mk_input(b, 5, 0, 0, 0, 0, 1234, 30000, -5, -30000, 18); gip_parse(&st, b, n, &ack);
    CHECK(st.c.s1_x == 1234 && st.c.s1_y == 30000 && st.c.s2_x == -5 && st.c.s2_y == -30000);

    /* Series X|S report is longer (share button etc.); same first 18 bytes */
    n = mk_input(b, 6, 0x10, 0, 0, 0, 0, 0, 0, 0, 48); b[48 - 26] = 0x01;
    ev = gip_parse(&st, b, n, &ack); CHECK(ev == GIP_EV_INPUT && st.c.a);

    /* too short: ignored, state kept */
    unsigned shorts = st.n_short;
    n = mk_input(b, 7, 0x20, 0, 0, 0, 0, 0, 0, 0, 18);
    ev = gip_parse(&st, b, 17, &ack);
    CHECK(ev == 0 && st.c.a && !st.c.b && st.n_short == shorts + 1);
    CHECK(gip_parse(&st, b, 3, &ack) == 0);
    CHECK(gip_parse(&st, NULL, 0, &ack) == 0);

    /* guide press asking for ack (One S), then release without ack */
    uint8_t g1[] = { 0x07, 0x30, 0x42, 0x02, 0x01, 0x5b };
    uint8_t g0[] = { 0x07, 0x20, 0x43, 0x02, 0x00, 0x5b };
    static const uint8_t want_ack[] = { 0x01, 0x20, 0x42, 0x09, 0x00, 0x07, 0x20, 0x02, 0, 0, 0, 0, 0 };
    ev = gip_parse(&st, g1, sizeof(g1), &ack);
    CHECK(ev == (GIP_EV_GUIDE | GIP_EV_ACK) && st.c.logo == 1 && st.c.a == 1);
    CHECK(ack.len == 13 && !memcmp(ack.data, want_ack, 13));
    /* input while guide held keeps logo */
    n = mk_input(b, 8, 0, 0, 0, 0, 0, 0, 0, 0, 18); gip_parse(&st, b, n, &ack);
    CHECK(st.c.logo == 1 && !st.c.a);
    ev = gip_parse(&st, g0, sizeof(g0), &ack);
    CHECK(ev == GIP_EV_GUIDE && st.c.logo == 0);
    uint8_t g2[] = { 0x07, 0x20, 0x44, 0x02, 0x02, 0x5b };          /* Elite 2 uses value 2 */
    gip_parse(&st, g2, sizeof(g2), &ack); CHECK(st.c.logo == 1);

    /* announce */
    uint8_t ann[32] = { 0x02, 0x20, 0x01, 0x1c, 0x7e, 0xed, 0x8b, 0x11, 0x0f, 0xa8, 0, 0, 0x5e, 0x04, 0x12, 0x0b };
    ev = gip_parse(&st, ann, sizeof(ann), &ack);
    CHECK(ev == GIP_EV_ANNOUNCE && st.ann_vid == 0x045e && st.ann_pid == 0x0b12);
    CHECK(gip_parse(&st, ann, 16, &ack) == 0);      /* truncated announce */
    /* status packet with ack requested -> generic ack, no state change */
    uint8_t stat[] = { 0x03, 0x30, 0x09, 0x04, 0x80, 0, 0, 0 };
    ev = gip_parse(&st, stat, sizeof(stat), &ack);
    CHECK(ev == (GIP_EV_ACK | GIP_EV_STATUS) && ack.data[2] == 0x09 && ack.data[5] == 0x03 && ack.data[7] == 4);
    /* chunked identify reply (riffmaster capture): 04 f0 02 3a eb 01 + 58 bytes;
     * ack must be 01 20 02 09 00 04 20 3a 00 00 00 b1 00 (XboxInput/xone) */
    uint8_t chunk[64] = { 0x04, 0xf0, 0x02, 0x3a, 0xeb, 0x01 };
    static const uint8_t want_cack[] = { 0x01, 0x20, 0x02, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0xb1, 0x00 };
    int used = 0;
    ev = gip_parse_ex(&st, chunk, sizeof(chunk), &ack, &used);
    CHECK(ev == (GIP_EV_ACK | GIP_EV_IDENTIFY) && used == 64 && st.chunk_total == 235);
    CHECK(ack.len == 13 && !memcmp(ack.data, want_cack, 13));
    uint8_t chunk2[64] = { 0x04, 0xb0, 0x03, 0x3a, 0x3a };      /* next chunk at offset 58 */
    ev = gip_parse_ex(&st, chunk2, sizeof(chunk2), &ack, &used);
    CHECK((ev & GIP_EV_ACK) && ack.data[7] == 116 && ack.data[11] == 119 && !(ev & GIP_EV_IDENTIFY_DONE));
    uint8_t chunk_end[] = { 0x04, 0xa0, 0x06, 0x00, 0xeb, 0x01 };   /* zero-length last chunk */
    ev = gip_parse_ex(&st, chunk_end, sizeof(chunk_end), &ack, &used);
    CHECK((ev & GIP_EV_IDENTIFY_DONE) && !(ev & GIP_EV_ACK) && used == 6);
    gip_hdr_t hh;
    uint8_t h2[] = { 0x04, 0xf0, 0x02, 0x81, 0x01, 0xeb, 0x01 };
    CHECK(gip_decode_header(h2, sizeof(h2), &hh) && hh.plen == 129 && hh.coff == 235 && hh.hlen == 7);
    /* two packets in one transfer: guide release + input */
    uint8_t two[80] = { 0x07, 0x20, 0x50, 0x02, 0x00, 0x5b };
    mk_input(two + 6, 9, 0x40, 0, 0, 0, 0, 0, 0, 0, 18);
    ev = gip_parse_ex(&st, two, 24, &ack, &used);
    CHECK(ev == GIP_EV_GUIDE && used == 6);
    ev = gip_parse_ex(&st, two + used, 24 - used, &ack, &used);
    CHECK(ev == GIP_EV_INPUT && st.c.x && used == 18);
    uint8_t pad0[8] = { 0 };
    CHECK(gip_parse_ex(&st, pad0, 8, &ack, &used) == 0 && used == 0);
    uint8_t lng[] = { 0x04, 0x30, 0x02, 0x81, 0x01, 0x00 };          /* 2-byte LEB128 length */
    CHECK(gip_parse(&st, lng, sizeof(lng), &ack) == 0);
    /* internal-flagged 0x20 is not an input report */
    n = mk_input(b, 9, 0x10, 0, 0, 0, 0, 0, 0, 0, 18); b[1] = 0x20;
    CHECK(gip_parse(&st, b, n, &ack) == 0);
}

/* ============================================================ driver tests */
static void reset_mock(void)
{
    inflight_in = inflight_out = inflight_in2 = NULL; n_out = 0;
    n_setconfig = n_setif = n_pipes = n_setif_any = 0; last_setif_alt = last_setif_if = -1;
}

static void test_match(void)
{
    usbdev_t *d;
    d = mkdev(0x045e, 0x02ea, 0xff, 0x47, 0xd0, cfg_one_s, sizeof(cfg_one_s));
    CHECK(usb_find_driver(d) == &usbgip_driver); freedev(d);
    d = mkdev(0x045e, 0x0b12, 0xff, 0x47, 0xd0, cfg_one_s, sizeof(cfg_one_s));
    CHECK(usb_find_driver(d) == &usbgip_driver); freedev(d);
    /* PowerA Series pad, not in the PID table: class fallback (device) */
    d = mkdev(0x20d6, 0x2009, 0xff, 0x47, 0xd0, cfg_one_s, sizeof(cfg_one_s));
    CHECK(usb_find_driver(d) == &usbgip_driver); freedev(d);
    /* class 0 on the device, GIP on interface 0 */
    d = mkdev(0x0e6f, 0x02a4, 0x00, 0x00, 0x00, cfg_one_s, sizeof(cfg_one_s));
    CHECK(usb_find_driver(d) == &usbgip_driver); freedev(d);
    /* 360 pads still go to usbctrl */
    d = mkdev(0x045e, 0x028e, 0xff, 0xff, 0xff, cfg_360, sizeof(cfg_360));
    CHECK(usb_find_driver(d) == &usbctrl_driver); freedev(d);
    d = mkdev(0x1234, 0x5678, 0xff, 0xff, 0xff, cfg_360, sizeof(cfg_360));   /* generic 360-protocol pad */
    CHECK(usb_find_driver(d) == &usbctrl_driver); freedev(d);
    /* wireless adapter / random vendor device: not GIP */
    d = mkdev(0x045e, 0x02fe, 0xff, 0x47, 0xd0, cfg_one_s, sizeof(cfg_one_s));
    CHECK(usb_find_driver(d) == &dummy_driver); freedev(d);
    d = mkdev(0x1234, 0x0001, 0xff, 0x00, 0x00, cfg_other, sizeof(cfg_other));
    CHECK(usb_find_driver(d) == NULL); freedev(d);
}

static usbdev_t *plug(uint16_t vid, uint16_t pid)
{
    usbdev_t *d = mkdev(vid, pid, 0xff, 0x47, 0xd0, cfg_one_s, sizeof(cfg_one_s));
    usb_driver_t *drv = usb_find_driver(d);
    CHECK(drv == &usbgip_driver);
    d->ud_drv = drv;
    drv->udrv_attach(d, drv);
    return d;
}

static const uint8_t ANN_0B12[32] = { 0x02, 0x20, 0x01, 0x1c, 0x7e, 0xed, 0x8b, 0x11, 0x0f, 0xa8, 0, 0,
                                       0x5e, 0x04, 0x12, 0x0b };

/* the Windows / XboxInput sequence for a Microsoft pad (host mode):
 * announce -> IDENTIFY, 4 chunks of the 235-byte identify reply (acked),
 * zero-length last chunk -> locale POWER, power-on, LED */
static void host_handshake(int *first_out)
{
    uint8_t c1[64] = { 0x04, 0xf0, 0x02, 0x3a, 0xeb, 0x01 };
    uint8_t c2[64] = { 0x04, 0xb0, 0x03, 0x3a, 0x3a };
    uint8_t c3[64] = { 0x04, 0xb0, 0x04, 0x3a, 0x74 };
    uint8_t c4[64] = { 0x04, 0xb0, 0x05, 0x3a, 0xae, 0x01 };
    uint8_t c5[] = { 0x04, 0xa0, 0x06, 0x00, 0xeb, 0x01 };
    int k = n_out;
    in_pkt(ANN_0B12, sizeof(ANN_0B12));
    CHECK(n_out == k + 1 && out_len[k] == 4 && out_log[k][0] == 0x04 && out_log[k][1] == 0x20 &&
          out_log[k][3] == 0x00);                                        /* IDENTIFY */
    drain_out();
    k = n_out;
    in_pkt(c1, 64); drain_out();
    in_pkt(c2, 64); drain_out();
    in_pkt(c3, 64); drain_out();
    in_pkt(c4, 64); drain_out();
    CHECK(n_out == k + 4);                                               /* 4 chunk acks */
    CHECK(out_log[k][0] == 0x01 && out_log[k][2] == 0x02 && out_log[k][5] == 0x04 &&
          out_log[k][7] == 0x3a && out_log[k][11] == 0xb1);
    CHECK(out_log[k + 3][7] == 0xe8 && out_log[k + 3][11] == 0x03);      /* 232 received, 3 left */
    k = n_out;
    in_pkt(c5, sizeof(c5));
    drain_out();
    CHECK(n_out == k + 3);
    CHECK(out_len[k] == 19 && out_log[k][0] == 0x05 && out_log[k][3] == 0x0f && out_log[k][11] == 'U');
    CHECK(out_len[k + 1] == 5 && out_log[k + 1][0] == 0x05 && out_log[k + 1][4] == 0x00);
    CHECK(out_len[k + 2] == 7 && out_log[k + 2][0] == 0x0a);
    if (first_out) *first_out = k;
}

static void test_driver_lifecycle(void)
{
    struct controller_data_s c;
    uint8_t b[GIP_MAX_PKT];
    long base_allocs = live_allocs;
    reset_mock();

    /* ---- host mode, Series X|S (the user's pad) */
    usbdev_t *d = plug(0x045e, 0x0b12);
    CHECK(d->ud_private != NULL);
    CHECK(n_setconfig == 1 && n_setif == 0 && n_pipes == 2);           /* no SET_INTERFACE */
    CHECK(last_ival[0] == 4 && last_ival[1] == 4);                      /* bInterval honoured */
    CHECK(log_has("pad0 connected 045e:0b12"));
    /* early power-on exactly 05 20 00 01 00, IN not queued before it completes */
    CHECK(n_out == 1 && out_len[0] == 5 && !memcmp(out_log[0], "\x05\x20\x00\x01\x00", 5));
    CHECK(inflight_in == NULL);
    complete_out_ok();
    CHECK(inflight_in && inflight_in->ur_length == 64 && (inflight_in->ur_flags & UR_FLAG_SHORTOK));
    CHECK(inflight_in->ur_pipe == d->ud_pipes[USB_EPADDR_TO_IDX(0x82)]);
    CHECK(log_has("read queued (early power-on done)"));

    /* pad keeps announcing until it sees IDENTIFY: re-sent at most once a second */
    in_pkt(ANN_0B12, sizeof(ANN_0B12));
    CHECK(n_out == 2 && out_log[1][0] == 0x04 && out_log[1][2] == 1);
    drain_out();
    in_pkt(ANN_0B12, sizeof(ANN_0B12));
    CHECK(n_out == 2);                                                  /* < 1 s: not again */
    mock_now_ms += 1100;
    in_pkt(ANN_0B12, sizeof(ANN_0B12));
    CHECK(n_out == 3 && out_log[2][0] == 0x04);
    drain_out();
    mock_now_ms += 1100;
    host_handshake(NULL);
    CHECK(log_has("init ok"));

    /* input report -> slot 0 */
    int n = mk_input(b, 1, 0x10, 0x01, 1023, 0, 100, 20000, -100, -20000, 18);
    in_pkt(b, n);
    CHECK(log_has("first input"));
    CHECK(get_controller_data(&c, 0) == 1);
    CHECK(c.a && c.up && c.lt == 255 && c.s1_x == 100 && c.s1_y == 20000 && c.s2_y == -20000 && !c.logo);
    CHECK(get_controller_data(&c, 0) == 0 && c.a);

    /* guide with ack: published + ack sent */
    int before = n_out;
    uint8_t g1[] = { 0x07, 0x30, 0x42, 0x02, 0x01, 0x5b };
    in_pkt(g1, sizeof(g1));
    CHECK(n_out == before + 1 && out_log[before][0] == 0x01 && out_log[before][2] == 0x42 &&
          out_log[before][5] == 0x07 && out_log[before][7] == 0x02 && out_len[before] == 13);
    CHECK(get_controller_data(&c, 0) == 1 && c.logo && c.a);
    drain_out();

    /* pad reboots its GIP side: re-announce restarts the handshake */
    before = n_out;
    in_pkt(ANN_0B12, sizeof(ANN_0B12));
    CHECK(log_has("restarting handshake") && n_out == before + 1 && out_log[before][0] == 0x04);
    drain_out();

    /* rumble through the libxenon API */
    before = n_out;
    set_controller_rumble(0, 200, 100);
    CHECK(n_out == before + 1 && out_log[before][0] == 0x09 && out_log[before][8] == 100);
    drain_out();
    CHECK(usbgip_set_rumble(3, 1, 1) == -1);

    /* OUT ring overflow while the pad never completes: drops, no crash */
    set_controller_rumble(0, 1, 1);
    for (int i = 0; i < 40; i++) set_controller_rumble(0, 1, 1);
    CHECK(inflight_out != NULL);

    /* USB errors on IN: counted, request re-queued; underrun still parsed */
    complete(&inflight_in, UR_ERR_DEVICENOTRESPONDING, NULL, 0);
    CHECK(inflight_in != NULL);
    n = mk_input(b, 2, 0x20, 0, 0, 0, 0, 0, 0, 0, 18);
    complete(&inflight_in, UR_ERR_DATAUNDERRUN, b, n);
    CHECK(get_controller_data(&c, 0) == 1 && c.b && !c.a && c.logo);

    /* no allocation per packet */
    long mid = total_allocs;
    for (int i = 0; i < 100000; i++) {
        n = mk_input(b, (uint8_t)i, (uint8_t)(i & 0xf0), 0, i & 1023, 0, (int16_t)i, 0, 0, 0, 18);
        in_pkt(b, n);
    }
    CHECK(total_allocs == mid);

    unplug(d);
    CHECK(live_allocs == base_allocs);
    CHECK(slot_mask == 0);
    CHECK(get_controller_data(&c, 0) == 1 && !c.a && !c.b && !c.logo && c.s1_x == 0);
    freedev(d);

    /* ---- 02d1 (2013 pad): host mode, read first, no early power-on */
    reset_mock();
    d = plug(0x045e, 0x02d1);
    CHECK(n_out == 0 && inflight_in != NULL);
    host_handshake(NULL);
    unplug(d); freedev(d);

    /* ---- third-party pad: xpad mode, SET_INTERFACE + xpad list at attach */
    reset_mock();
    d = plug(0x0e6f, 0x02a4);
    CHECK(n_setif == 1 && inflight_in != NULL);
    CHECK(n_out == 1 && out_log[0][0] == 0x05 && out_log[0][2] == 1);
    CHECK(drain_out() == 3 && out_log[1][0] == 0x0a && out_log[2][0] == 0x06);
    CHECK(log_has("init ok"));
    uint8_t ann_pdp[32] = { 0x02, 0x20, 0x01, 0x1c };
    in_pkt(ann_pdp, sizeof(ann_pdp));
    CHECK(drain_out() == 3);                                            /* xpad: re-init on announce */
    unplug(d); freedev(d);
    CHECK(live_allocs == base_allocs);
}

static void test_replug_and_slots(void)
{
    struct controller_data_s c;
    uint8_t b[GIP_MAX_PKT];
    long base = live_allocs;

    /* 1000 plug/unplug cycles: no leak, always slot 0 */
    for (int i = 0; i < 1000; i++) {
        reset_mock();
        usbdev_t *d = plug(0x045e, 0x02d1);
        if (i & 1) drain_out();                /* sometimes unplug mid-init */
        unplug(d);
        freedev(d);
        if (live_allocs != base || slot_mask != 0) { CHECK(0); break; }
    }
    CHECK(live_allocs == base);

    /* 360 pad holds slot 0 -> One pad gets slot 1; second One pad slot 2 */
    reset_mock();
    slot_mask = 1;                              /* a wired 360 pad on slot 0 */
    usbdev_t *d1 = plug(0x045e, 0x0b12);
    CHECK(((usbdev_t *)d1)->ud_private && log_has("pad1 connected 045e:0b12"));
    drain_out();
    int n = mk_input(b, 1, 0x40, 0, 0, 0, 0, 0, 0, 0, 18);
    in_pkt(b, n);
    CHECK(get_controller_data(&c, 1) == 1 && c.x);
    usbreq_t *keep_in = inflight_in; inflight_in = NULL;   /* park d1's IN */
    usbdev_t *d2 = plug(0x20d6, 0x2009);
    CHECK(log_has("pad2 connected 20d6:2009"));
    drain_out();
    CHECK(slot_mask == 7);
    unplug(d2); freedev(d2);
    CHECK(slot_mask == 3);
    inflight_in = keep_in;
    unplug(d1); freedev(d1);
    CHECK(slot_mask == 1);

    /* all 4 slots busy at attach: pad attaches with no slot, takes one when freed */
    slot_mask = 0xf;
    reset_mock();
    usbdev_t *d3 = plug(0x045e, 0x02dd);
    drain_out();
    slot_mask = 0xb;                            /* slot 2 freed */
    n = mk_input(b, 1, 0x80, 0, 0, 0, 0, 0, 0, 0, 18);
    in_pkt(b, n);
    CHECK(get_controller_data(&c, 2) == 1 && c.y);
    unplug(d3); freedev(d3);
    CHECK(slot_mask == 0xb);
    slot_mask = 0;
    CHECK(live_allocs == base);
}

static void test_retry_and_stuck(void)
{
    uint8_t b[GIP_MAX_PKT];
    long base = live_allocs;

    /* host mode, pad totally silent (the v3 hardware symptom): early power-on
     * re-sent every 3 s, 5 times */
    reset_mock();
    usbdev_t *d = plug(0x045e, 0x0b12);
    complete_out_ok();
    CHECK(n_out == 1 && inflight_in);
    usbgip_tick();
    CHECK(n_out == 1);
    for (int k = 0; k < 8; k++) { mock_now_ms += 3000; usbgip_tick(); drain_out(); }
    CHECK(n_out == 1 + 5);
    for (int i = 1; i < 6; i++) CHECK(out_len[i] == 5 && out_log[i][0] == 0x05 && out_log[i][2] == 0);
    CHECK(log_has("retry 5/5"));
    unplug(d); freedev(d);

    /* host mode, early power-on never completes: read starts anyway after 1 s */
    reset_mock();
    d = plug(0x045e, 0x0b12);
    CHECK(inflight_in == NULL);
    mock_now_ms += 1500; usbgip_tick();
    CHECK(inflight_in != NULL && log_has("not completed after 1 s"));
    CHECK(log_has("still not completed"));
    complete(&inflight_out, UR_ERR_STALL, NULL, 0);
    CHECK(log_has("FAILED: usb status 4"));
    unplug(d); freedev(d);

    /* xpad mode, silent pad: xpad list re-sent, 5 times */
    reset_mock();
    d = plug(0x0e6f, 0x02a4);
    drain_out();
    CHECK(n_out == 3);
    for (int k = 0; k < 8; k++) { mock_now_ms += 3000; usbgip_tick(); drain_out(); }
    CHECK(n_out == 3 + 5 * 3);
    /* xpad pad that reports input is not retried */
    int n = mk_input(b, 1, 0, 0, 0, 0, 0, 0, 0, 0, 18);
    in_pkt(b, n);
    int before = n_out;
    mock_now_ms += 5000; usbgip_tick();
    CHECK(n_out == before);
    unplug(d); freedev(d);
    CHECK(live_allocs == base);
}

/* Series X|S layout from the v3 hardware log: interface 0 has alt 0
 * (02/82, interval 4) and alt 1 (02/82, IN interval 2) */
static const uint8_t cfg_series[] = {
    9, 2, 0, 0, 3, 1, 0, 0xa0, 0xfa,
    9, 4, 0, 0, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x02, 3, 64, 0, 4,
      7, 5, 0x82, 3, 64, 0, 4,
    9, 4, 0, 1, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x02, 3, 64, 0, 4,
      7, 5, 0x82, 3, 64, 0, 2,
    9, 4, 1, 0, 0, 0xff, 0x47, 0xd0, 0,
    9, 4, 1, 1, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x03, 1, 228, 0, 1,
      7, 5, 0x83, 1, 64, 0, 1,
    9, 4, 2, 0, 0, 0xff, 0x47, 0xd0, 0,
    9, 4, 2, 1, 2, 0xff, 0x47, 0xd0, 0,
      7, 5, 0x04, 2, 64, 0, 0,
      7, 5, 0x84, 2, 64, 0, 0,
};

static usbdev_t *plug_series(void)
{
    usbdev_t *d = mkdev(0x045e, 0x0b12, 0xff, 0x47, 0xd0, cfg_series, sizeof(cfg_series));
    usb_driver_t *drv = usb_find_driver(d);
    CHECK(drv == &usbgip_driver);
    d->ud_drv = drv;
    drv->udrv_attach(d, drv);
    return d;
}

static void test_variants(void)
{
    long base = live_allocs;
    usbdev_t *d;

    usbgip_force_variant = 1;                   /* no-setcfg */
    reset_mock(); d = plug_series();
    CHECK(n_setconfig == 0 && n_setif_any == 0 && log_has("SET_CONFIGURATION skipped"));
    complete_out_ok(); CHECK(inflight_in && inflight_in->ur_length == 64);
    unplug(d); freedev(d);

    usbgip_force_variant = 2;                   /* setif0.0 */
    reset_mock(); d = plug_series();
    CHECK(n_setconfig == 1 && n_setif_any == 1 && last_setif_if == 0 && last_setif_alt == 0);
    unplug(d); freedev(d);

    usbgip_force_variant = 3;                   /* alt1: SET_INTERFACE(0,1) + alt-1 endpoints */
    reset_mock(); d = plug_series();
    CHECK(n_setif_any == 1 && last_setif_if == 0 && last_setif_alt == 1);
    CHECK(last_ival[1] == 2 && last_ival[0] == 4);
    unplug(d); freedev(d);

    usbgip_force_variant = 4;                   /* in32 */
    reset_mock(); d = plug_series(); complete_out_ok();
    CHECK(inflight_in && inflight_in->ur_length == 32);
    unplug(d); freedev(d);

    usbgip_force_variant = 5;                   /* in512: multi-packet read, parsed */
    reset_mock(); d = plug_series(); complete_out_ok();
    CHECK(inflight_in && inflight_in->ur_length == 512);
    {
        uint8_t big[512]; memset(big, 0, sizeof(big));
        memcpy(big, "\x02\x20\x01\x1c", 4);
        complete(&inflight_in, 0, big, 32);
        CHECK(inflight_in && inflight_in->ur_length == 512 && log_has("-> IDENTIFY"));
    }
    unplug(d); freedev(d);

    usbgip_force_variant = 6;                   /* 2in: two IN TDs queued */
    reset_mock(); d = plug_series(); complete_out_ok();
    CHECK(inflight_in && inflight_in2 && inflight_in != inflight_in2);
    {
        uint8_t b2[64]; int n = mk_input(b2, 1, 0x10, 0, 0, 0, 0, 0, 0, 0, 18);
        complete(&inflight_in2, 0, b2, n);       /* second one completes first */
        CHECK(inflight_in2 != NULL);             /* ... and is re-queued */
        struct controller_data_s c; CHECK(get_controller_data(&c, 0) == 1 && c.a);
    }
    unplug(d); freedev(d);                       /* both cancelled + freed */

    usbgip_force_variant = 7;                   /* seq1 */
    reset_mock(); d = plug_series();
    CHECK(n_out == 1 && out_len[0] == 5 && out_log[0][0] == 0x05 && out_log[0][2] == 1);
    unplug(d); freedev(d);

    /* cycling (hardware build): each plug-in takes the next of the 7 */
    usbgip_force_variant = -1;
    {
        static const char *nm[] = { "variant base, IN", "variant setif0.0, IN", "variant alt1, IN",
                                    "variant in32, IN", "variant in512, IN", "variant 2in, IN", "variant seq1, IN" };
        int seen[7] = { 0 }, all = 1;
        for (int i = 0; i < 7; i++) {
            reset_mock(); d = plug_series();
            for (int k = 0; k < 7; k++) if (usblog_find(nm[k], 15)) seen[k]++;
            unplug(d); freedev(d);
        }
        for (int k = 0; k < 7; k++) if (seen[k] != 1) all = 0;
        CHECK(all);                              /* 7 plug-ins = each variant exactly once */
    }

    /* raw dumps: every 250 ms during the first minute, then they stop */
    usbgip_force_variant = 0;
    reset_mock(); d = plug_series(); complete_out_ok();
    n_dump_calls = 0;
    for (int i = 0; i < 8; i++) { mock_now_ms += 250; usbgip_tick(); }
    CHECK(n_dump_calls >= 8);
    CHECK(log_has("dump pad0 v=base"));
    int before = n_dump_calls;
    mock_now_ms += 70000; usbgip_tick(); mock_now_ms += 250; usbgip_tick();
    CHECK(n_dump_calls == before);
    unplug(d); freedev(d);
    CHECK(live_allocs == base);
}

static void test_replay(void)
{
    /* nothing reaches the pipe before the first usblog_tick */
    CHECK(n_np == 0);
    CHECK(usblog_total() > 96);            /* earlier tests filled boot + recent */

    /* boot-like situation: a pad attached long before the pipe exists */
    int max_calls = 0, ticks = 0;
    long base = live_allocs;
    reset_mock();
    usbdev_t *d = plug(0x045e, 0x02ea);
    drain_out();
    for (int i = 0; i < 200000; i++) {
        mock_now_ms += 1;
        np_calls_this_tick = 0;
        if (i == 7000) np_inited = 1;              /* net up at ~+7 s */
        if (i < 50000) usblog_quiet("flood %d", i); /* far more than the history holds */
        if (i % 3000 == 0) usbgip_tick();
        usblog_tick();
        ticks++;
        if (np_calls_this_tick > max_calls) max_calls = np_calls_this_tick;
        /* single consumer on the same thread, but stalled 20..40 s */
        if (!(i >= 20000 && i < 40000)) np_consume(8);
    }
    CHECK(ticks == 200000);                        /* never wedged */
    CHECK(max_calls <= 1);                         /* at most one line per poll */
    CHECK(np_drops > 0);                           /* ring was full during the stall ... */
    CHECK(np_delivered > 500);                     /* ... and delivery resumed */
    CHECK(np_saw_sum >= 5);             /* every 2 s, or every 30 s after the first 2 min */
    CHECK(np_saw_enum > 0);                        /* boot enumeration replayed after net up */
    CHECK(np_saw_boot0 > 0);
    CHECK(usblog_dropped() > 0);                   /* flood overwrote unsent recent lines, counted */
    CHECK(strstr(np_last, "[gip]") != NULL);
    unplug(d); freedev(d);
    CHECK(live_allocs == base);

    /* the summary line carries the enumeration and the pad state */
    np_consume(64);
    reset_mock();
    d = plug(0x045e, 0x0b12);
    drain_out();
    mock_now_ms += 60000;
    for (int i = 0; i < 10 && !strstr(np_last, "sum#"); i++) { mock_now_ms += 5; usblog_tick(); np_consume(8); }
    CHECK(strstr(np_last, "045e:0b12>GIP") != NULL);
    CHECK(strstr(np_last, "p0 045e:0b12 v=base H") != NULL);
    unplug(d); freedev(d);

    usblog_bc("bc crash-tail marker %d", 7);
    const char *tail = usblog_crash_tail();
    CHECK(tail && strstr(tail, "bc crash-tail marker 7") && strlen(tail) < 12 * 101 + 64);
    CHECK(strstr(tail, "oldest first") != NULL);
    char hex[64];
    uint8_t x[] = { 0x05, 0x20, 0xab };
    usblog_hex(hex, x, 3, 16);
    CHECK(!strcmp(hex, "05 20 ab"));
}

static void test_bad_descriptors(void)
{
    /* GIP-class device without interrupt endpoints on interface 0 */
    static const uint8_t cfg_bad[] = {
        9, 2, 0, 0, 1, 1, 0, 0xa0, 0xfa,
        9, 4, 0, 0, 0, 0xff, 0x47, 0xd0, 0,
        9, 4, 1, 0, 1, 0xff, 0x47, 0xd0, 0,
          7, 5, 0x81, 3, 64, 0, 4,
    };
    long base = live_allocs;
    reset_mock();
    usbdev_t *d = mkdev(0x045e, 0x02ea, 0xff, 0x47, 0xd0, cfg_bad, sizeof(cfg_bad));
    usb_driver_t *drv = usb_find_driver(d);
    d->ud_drv = drv;
    drv->udrv_attach(d, drv);
    CHECK(d->ud_private == NULL && slot_mask == 0 && live_allocs == base);
    drv->udrv_detach(d);
    freedev(d);
    /* truncated / zero-length descriptor must not loop forever */
    static const uint8_t cfg_zero[] = { 9, 2, 0, 0, 1, 1, 0, 0xa0, 0xfa, 0, 4, 0, 0 };
    d = mkdev(0x045e, 0x02ea, 0xff, 0x47, 0xd0, cfg_zero, sizeof(cfg_zero));
    d->ud_drv = &usbgip_driver;
    usbgip_driver.udrv_attach(d, &usbgip_driver);
    CHECK(d->ud_private == NULL);
    freedev(d);
}

#if !GIP_DEBUG
/* production build (installed libxenon): behaviour without the debug lines */
static void test_prod(void)
{
    struct controller_data_s c;
    uint8_t b[GIP_MAX_PKT];
    long base = live_allocs;
    usbgip_force_variant = -1;                         /* the real production default */
    for (int round = 0; round < 3; round++) {          /* no variant cycling: same every plug */
        reset_mock();
        usbdev_t *d = plug(0x045e, 0x0b12);
        CHECK(n_setconfig == 1 && n_setif == 0 && last_ival[1] == 4);
        CHECK(n_out == 1 && !memcmp(out_log[0], "\x05\x20\x00\x01\x00", 5) && inflight_in == NULL);
        complete_out_ok();
        CHECK(inflight_in && inflight_in->ur_length == 64);
        host_handshake(NULL);
        int n = mk_input(b, 1, 0x10, 0x01, 1023, 0, 100, 20000, -100, -20000, 18);
        in_pkt(b, n);
        CHECK(get_controller_data(&c, 0) == 1 && c.a && c.up && c.lt == 255 && c.s1_y == 20000);
        long mid = total_allocs;
        for (int i = 0; i < 20000; i++) { n = mk_input(b, (uint8_t)i, 0, 0, 0, 0, 0, 0, 0, 0, 18); in_pkt(b, n); }
        CHECK(total_allocs == mid);
        unplug(d); freedev(d);
        CHECK(live_allocs == base && slot_mask == 0);
    }
    /* summary every 30 s, not every 2 s */
    np_inited = 1; np_consume(64);
    int before = np_saw_sum;
    for (int i = 0; i < 20000; i++) { mock_now_ms += 1; usblog_tick(); np_consume(8); }
    CHECK(np_saw_sum - before <= 2);
    for (int i = 0; i < 40000; i++) { mock_now_ms += 1; usblog_tick(); np_consume(8); }
    CHECK(np_saw_sum - before >= 1);
    usblog_bc("bc prod marker %d", 3);                  /* crash ring only */
    CHECK(strstr(usblog_crash_tail(), "bc prod marker 3") != NULL);
}
#endif

int main(void)
{
    usbgip_force_variant = 0;    /* v4.2 behaviour for the classic tests */
#if !GIP_DEBUG
    test_init_builder(); test_seq(); test_host_builders(); test_ack_rumble(); test_parse(); test_match();
    test_prod(); test_bad_descriptors();
    printf("%d checks, %d failures (production build)\n", checks, fails);
    return fails ? 1 : 0;
#endif
    test_init_builder();
    test_seq();
    test_host_builders();
    test_ack_rumble();
    test_parse();
    test_match();
    test_driver_lifecycle();
    test_replug_and_slots();
    test_bad_descriptors();
    test_retry_and_stuck();
    test_variants();
    test_replay();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
