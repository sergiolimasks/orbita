/*
 * XCloud360 — inputdiag
 *
 * Diagnostic for controller input on bare-metal libxenon. Answers:
 *
 *   1. Which USB devices are ENUMERATED on the console's OHCI buses?
 *      (walks usb_buses[] + ub_devices[] — bypasses the driver-table match
 *      so we see EVERY device, including the internal RF module which is
 *      wired to the southbridge via internal USB pins per Free60 wiki.)
 *
 *   2. Which of those devices got a driver attached (ud_drv != NULL)?
 *      (a device visible but NOT driven = missing entry in usbdevs.c)
 *
 *   3. Does get_controller_data() ever return non-zero over ~20 s?
 *      (this is what hwtest exercises — same code path libxenon apps use)
 *
 *   4. Does the SMC surface anything when the XBOX button on a wireless
 *      pad is pressed? (SMC 0x11/0x20 = power, 0x23 = IR from media remote —
 *      wireless pads route the XBOX-button power event through the SMC)
 *
 * Motivation: the test controller is a wireless Xbox 360 pad paired with the
 * console's INTERNAL RF module. libxenon's usbctrl.c only reacts to a fixed
 * set of USB VID/PID pairs (see usbdevs.c). We do NOT know a priori which
 * VID/PID the internal RF module presents to the OHCI stack. This program
 * prints that so we can either add the ID and drive it, or confirm the RF
 * module needs kernel/XAM support (XEX path).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_smc/xenon_smc.h>
#include <ppc/timebase.h>
#include <input/input.h>
#include <usb/usbmain.h>
/* usbd.h references queue_t + physaddr_t from CFE / nocfe headers */
#include <nocfe/lib_types.h>
#include <nocfe/lib_queue.h>
#include <nocfe/cfe.h>
#include <usb/usbd.h>
#include <usb/usbchap9.h>

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.0.1-inputdiag"
#endif

/* Match libxenon usbmain.c internals. */
extern int usb_buscnt;
extern usbbus_t *usb_buses[];   /* USB_MAX_BUS == 4 */

static void tb_udelay(uint64_t usec) {
    uint64_t start = mftb();
    uint64_t ticks = usec * (PPC_TIMEBASE_FREQ / 1000000ULL);
    while ((mftb() - start) < ticks) { }
}

/*
 * Known VID/PID mapping — synced with libxenon usbdevs.c + Microsoft's public
 * Xbox 360 accessory PIDs. If an ID is present here but NOT in usbdevs.c,
 * the device shows up on the bus but no driver is bound.
 */
struct id_note {
    uint16_t vid, pid;
    const char *name;
    int in_libxenon_table;   /* 1 = usbdevs.c matches it */
};

static const struct id_note KNOWN_IDS[] = {
    { 0x045e, 0x028e, "Xbox 360 Wired Controller",              1 },
    { 0x045e, 0x0291, "Xbox 360 Wireless Receiver (variant)",   0 },
    { 0x045e, 0x02a9, "Xbox 360 Wireless Controller (via recv)",1 },
    { 0x045e, 0x02aa, "Xbox 360 Wireless Controller (via recv)",1 },
    { 0x045e, 0x0719, "Xbox 360 Wireless Gaming Receiver (USB)",0 },
    { 0x045e, 0x028f, "Xbox 360 Play & Charge Kit",             1 /*dummy*/ },
    { 0x045e, 0x02b0, "Xbox 360 Kinect",                        1 /*dummy*/ },
    { 0x1bad, 0xf900, "PDP Afterglow Controller",               1 },
    { 0x0000, 0x0000, NULL, 0 }
};

static const char *lookup_id(uint16_t vid, uint16_t pid, int *in_table) {
    for (const struct id_note *k = KNOWN_IDS; k->name; k++) {
        if (k->vid == vid && k->pid == pid) {
            *in_table = k->in_libxenon_table;
            return k->name;
        }
    }
    *in_table = 0;
    return NULL;
}

static void dump_devices(void) {
    printf("[USB] usb_buscnt=%d\n", usb_buscnt);
    int total = 0, driven = 0, unknown = 0;

    for (int b = 0; b < usb_buscnt; b++) {
        usbbus_t *bus = usb_buses[b];
        if (!bus) continue;
        printf("[USB] bus[%d] ub_num=%d\n", b, bus->ub_num);
        for (int addr = 0; addr < USB_MAX_DEVICES; addr++) {
            usbdev_t *dev = bus->ub_devices[addr];
            if (!dev) continue;
            usb_device_descr_t *d = &dev->ud_devdescr;
            uint16_t vid = GETUSBFIELD(d, idVendor);
            uint16_t pid = GETUSBFIELD(d, idProduct);
            int in_table = 0;
            const char *name = lookup_id(vid, pid, &in_table);
            const char *drvname = dev->ud_drv ? "DRIVEN" : "no-driver";
            total++;
            if (dev->ud_drv) driven++;
            if (!name) unknown++;

            printf("  addr=%3d VID=%04x PID=%04x class=%02x/%02x/%02x  %s  %s%s\n",
                   addr, vid, pid,
                   d->bDeviceClass, d->bDeviceSubClass, d->bDeviceProtocol,
                   drvname,
                   name ? name : "(unknown VID/PID — NOT in libxenon usbdevs.c)",
                   (name && !in_table) ? "  [known but UNBOUND]" : "");
        }
    }
    printf("[USB] total=%d driven=%d unknown=%d\n", total, driven, unknown);
    if (unknown > 0) {
        printf("[USB] ** unknown device(s) on the bus — potential RF-module ID we need to add\n");
        printf("[USB] ** Photograph the VID:PID above and add to libxenon usbdevs.c\n");
    }
}

int main(void) {
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("========================================\n");
    printf(" XCloud360 inputdiag — RF/wireless probe\n");
    printf(" build:    %s\n", XCLOUD360_BUILD_VERSION);
    printf(" built:    %s %s\n", __DATE__, __TIME__);
    printf("========================================\n");

    printf("[USB] usb_init()...\n");
    int urc = usb_init();
    printf("[USB] usb_init returned %d\n", urc);

    /* Give the OHCI stack time to enumerate the root hub + any downstream
     * devices (including the internal RF module port on the southbridge). */
    printf("[USB] polling for 3s to let enumeration settle...\n");
    for (int i = 0; i < 60; i++) {
        usb_do_poll();
        tb_udelay(50 * 1000);
    }

    dump_devices();

    printf("========================================\n");
    printf(" Now testing input paths for 20 seconds.\n");
    printf(" Press EVERY BUTTON on your wireless pad,\n");
    printf(" including the CENTRAL XBOX button.\n");
    printf(" Also try pressing the console's power button.\n");
    printf("========================================\n");

    uint64_t start_tb = mftb();
    int last_report_sec = 0;
    int pad_events = 0;
    int smc_ir_events = 0;
    /* We also count how many "power messages" the SMC surfaces — those
     * often come from the XBOX button when the pad is on. libxenon has no
     * public API to intercept them cleanly, but its handle_bulk prints
     * "SMC power message" to uprintf, which surfaces on the console. */

    for (;;) {
        usb_do_poll();

        /* USB controller path (this is what hwtest and games use). */
        for (int p = 0; p < 4; p++) {
            struct controller_data_s pad;
            if (get_controller_data(&pad, p)) {
                if (pad.a || pad.b || pad.x || pad.y
                    || pad.start || pad.back || pad.logo
                    || pad.up || pad.down || pad.left || pad.right
                    || pad.lb || pad.rb || pad.lt || pad.rt) {
                    pad_events++;
                    printf("[PAD %d] a=%d b=%d x=%d y=%d start=%d back=%d logo=%d "
                           "d=%d%d%d%d lb=%d rb=%d lt=%u rt=%u s1=(%d,%d) s2=(%d,%d)\n",
                           p, pad.a, pad.b, pad.x, pad.y,
                           pad.start, pad.back, pad.logo,
                           pad.up, pad.down, pad.left, pad.right,
                           pad.lb, pad.rb, pad.lt, pad.rt,
                           pad.s1_x, pad.s1_y, pad.s2_x, pad.s2_y);
                }
            }
        }

        /* SMC IR (media remote + XBOX-button power routing). */
        int ir = xenon_smc_get_ir();
        if (ir >= 0) {
            smc_ir_events++;
            printf("[SMC IR] code=0x%02x (media remote or pad power event)\n", ir);
        }

        /* One-second heartbeat. */
        uint64_t now = mftb();
        int sec = (int)(tb_diff_sec(now, start_tb));
        if (sec != last_report_sec) {
            last_report_sec = sec;
            printf("[t=%2ds] pad_events=%d smc_ir_events=%d\n",
                   sec, pad_events, smc_ir_events);
            if (sec >= 20) break;
        }

        tb_udelay(10 * 1000);
    }

    printf("========================================\n");
    printf(" SUMMARY\n");
    printf("   USB devices enumerated:   see above dump\n");
    printf("   pad events (usbctrl):     %d\n", pad_events);
    printf("   SMC IR events:            %d\n", smc_ir_events);
    printf("\n");
    if (pad_events == 0 && smc_ir_events == 0) {
        printf(" DIAGNOSIS:\n");
        printf("   No input received via libxenon's usbctrl or SMC.\n");
        printf("   Cross-reference with the USB device dump above:\n");
        printf("     - If wireless receiver VID:PID is DRIVEN but no events,\n");
        printf("       the pad may not be paired with the RF module.\n");
        printf("     - If VID:PID shows up UNKNOWN or UNBOUND, we need to\n");
        printf("       add that ID to libxenon usbdevs.c and rebuild.\n");
        printf("     - If NO Microsoft device appears at all, the internal\n");
        printf("       RF module is not enumerated on the OHCI bus in this\n");
        printf("       boot path — bare-metal libxenon may lack the internal\n");
        printf("       root-hub port init that XAM/kernel does.\n");
    } else if (pad_events > 0) {
        printf(" DIAGNOSIS: usbctrl path is WORKING — pad IS detected.\n");
    } else {
        printf(" DIAGNOSIS: only SMC events — usbctrl is silent, pad NOT bound.\n");
    }
    printf("========================================\n");
    printf(" Press console power button to exit.\n");

    /* Wait for anything, or just idle. */
    for (int i = 0; i < 600; i++) {
        usb_do_poll();
        tb_udelay(100 * 1000);
        int ir = xenon_smc_get_ir();
        if (ir >= 0) {
            printf("[SMC IR post-test] 0x%02x\n", ir);
        }
    }

    return 0;
}
