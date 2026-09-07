/*
 * XCloud360 — inputdiag-safe
 *
 * Post-mortem of P1.5-INPUT-001 (Machine Check on original inputdiag):
 * root cause was walking libxenon USB internals from user code. The libxenon
 * browser example uses only the public API (usb_init / usb_do_poll /
 * get_controller_data) and works with USB controllers — so we mirror that
 * exactly. NO manual iteration of usb_buses[], NO include of <usb/usbd.h>,
 * NO printf format with more than 4 args.
 *
 * Purpose: answer a single question — does libxenon's usbctrl path see the
 * wireless pad paired to the internal RF module? Yes/no. If no, ADR-006
 * decision matrix routes us to next steps.
 *
 * Spec §41 (test-failure protocol): capture → reduce → reproduce.
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
#include <usb/usbmain.h>       /* PUBLIC API ONLY */

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.0.2-inputdiag-safe"
#endif

static void tb_udelay(uint64_t usec) {
    uint64_t start = mftb();
    uint64_t ticks = usec * (PPC_TIMEBASE_FREQ / 1000000ULL);
    while ((mftb() - start) < ticks) { }
}

/* Small printf-heavy blocks are dangerous in newlib bare-metal when arg
 * pointers are stale. Keep every printf < 4 vararg args, no %s from anything
 * derived from library internals. */

int main(void) {
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("========================================\n");
    printf(" XCloud360 inputdiag-safe\n");
    printf(" build: %s\n", XCLOUD360_BUILD_VERSION);
    printf(" built: %s\n", __DATE__);
    printf("========================================\n");

    printf("[USB] init...\n");
    int urc = usb_init();
    printf("[USB] usb_init rc = %d\n", urc);

    /* Poll for 3s so enumeration + hub configuration settle.
     * Mirrors browser example exactly. */
    printf("[USB] settling (3s)...\n");
    for (int i = 0; i < 60; i++) {
        usb_do_poll();
        tb_udelay(50 * 1000);
    }
    printf("[USB] settle done.\n");

    printf("========================================\n");
    printf(" Press ANY button on your pad for 20s.\n");
    printf(" Include the central XBOX button.\n");
    printf(" Also try the console power button.\n");
    printf("========================================\n");

    uint64_t start_tb = mftb();
    int last_sec = -1;
    int pad_events[4] = { 0, 0, 0, 0 };
    int smc_ir_events = 0;
    int any_event_flag = 0;

    for (;;) {
        usb_do_poll();

        /* Poll all 4 ports via PUBLIC API — same call the browser example uses. */
        for (int p = 0; p < 4; p++) {
            struct controller_data_s pad;
            if (get_controller_data(&pad, p)) {
                /* Small OR to check for any activity. */
                int any = pad.a | pad.b | pad.x | pad.y
                        | pad.start | pad.back | pad.logo
                        | pad.up | pad.down | pad.left | pad.right
                        | pad.lb | pad.rb | pad.lt | pad.rt;
                if (any) {
                    pad_events[p]++;
                    /* Split into TWO short printfs — under 4 args each. */
                    printf("[PAD %d] ABXY %d%d%d%d\n",
                           p, pad.a, pad.b, pad.x, pad.y);
                    printf("[PAD %d] SBLo %d%d%d\n",
                           p, pad.start, pad.back, pad.logo);
                    any_event_flag = 1;
                }
            }
        }

        /* SMC IR — includes pad power / media remote events. */
        int ir = xenon_smc_get_ir();
        if (ir >= 0) {
            smc_ir_events++;
            printf("[SMC IR] code = 0x%02x\n", ir);
            any_event_flag = 1;
        }

        /* Per-second heartbeat. */
        uint64_t now = mftb();
        int sec = (int)tb_diff_sec(now, start_tb);
        if (sec != last_sec) {
            last_sec = sec;
            printf("[t=%d s] pad0=%d pad1=%d ir=%d\n",
                   sec, pad_events[0], pad_events[1], smc_ir_events);
            if (sec >= 20) break;
        }

        tb_udelay(10 * 1000);
    }

    printf("========================================\n");
    printf(" SUMMARY\n");
    printf(" pad0 events: %d\n", pad_events[0]);
    printf(" pad1 events: %d\n", pad_events[1]);
    printf(" pad2 events: %d\n", pad_events[2]);
    printf(" pad3 events: %d\n", pad_events[3]);
    printf(" smc ir events: %d\n", smc_ir_events);
    printf("========================================\n");

    if (any_event_flag) {
        printf(" DIAGNOSIS: usbctrl / SMC PATH WORKING.\n");
        printf(" Some input was seen.\n");
    } else {
        printf(" DIAGNOSIS: NO INPUT SEEN via usbctrl or SMC.\n");
        printf(" Interpretation:\n");
        printf(" - Wireless pad paired to internal RF module is\n");
        printf("   NOT visible to libxenon's usbctrl driver.\n");
        printf(" - Confirms ADR-006 branch (c): production input\n");
        printf("   needs XEX/kernel (XAM/XInput) OR wired pad OR\n");
        printf("   USB Wireless Gaming Receiver dongle.\n");
    }
    printf("========================================\n");
    printf(" Idle — press console power to exit.\n");

    for (int i = 0; i < 600; i++) {
        usb_do_poll();
        tb_udelay(100 * 1000);
        int ir = xenon_smc_get_ir();
        if (ir >= 0) {
            printf("[SMC IR idle] 0x%02x\n", ir);
        }
    }

    return 0;
}
