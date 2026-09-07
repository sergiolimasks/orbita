/*
 * XCloud360 — P1 hardware test (hwtest)
 *
 * Boots via XeLL (raw ELF32). Brings up console, prints:
 *   - build id / timestamp
 *   - PPC timebase sanity (delay measurement)
 *   - SMC sensor readings (temps / fan)
 *   - network (lwIP + DHCP)
 *   - controller poll (Xbox 360 pad)
 *
 * All output goes to the on-screen console (HDMI). No filesystem write —
 * XeLL runs from RAM, no reliable persistent log path at P1. User photographs
 * the screen or captures HDMI. P2 will add FTP-based log retrieval.
 *
 * Waits for BACK button to exit (graceful loop exit → XeLL reboot).
 *
 * Spec refs: §29 P1, §26 (logging), §40 (test protocol).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_smc/xenon_smc.h>
#include <ppc/timebase.h>
#include <network/network.h>
#include <input/input.h>
#include <usb/usbmain.h>

#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.0.1-p1"
#endif

/* Read PIR (Processor ID Register) SPR to get hardware thread id (0..5). */
static inline uint32_t read_pir(void) {
    uint32_t pir;
    asm volatile ("mfspr %0, 1023" : "=r" (pir));
    return pir;
}

/* Busy-wait for approximately N microseconds using PPC timebase. */
static void tb_udelay(uint64_t usec) {
    uint64_t start = mftb();
    uint64_t ticks = usec * (PPC_TIMEBASE_FREQ / 1000000ULL);
    while ((mftb() - start) < ticks) { /* spin */ }
}

int main(void) {
    /* Bring up video console. VIDEO_MODE_AUTO lets libxenon pick best output. */
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("========================================\n");
    printf(" XCloud360 hardware test (P1)\n");
    printf(" build:    %s\n", XCLOUD360_BUILD_VERSION);
    printf(" built:    %s %s\n", __DATE__, __TIME__);
    printf("========================================\n");

    /* CPU / threads. */
    uint32_t pir = read_pir();
    printf("[CPU] xenon PPC64 BE, 3 cores / 6 hw threads\n");
    printf("[CPU] main thread PIR=%u (hw_thread=%u, core=%u)\n",
           pir, pir & 0x1, (pir >> 1) & 0x3);
    printf("[CPU] PPC_TIMEBASE_FREQ=%llu Hz\n",
           (unsigned long long)PPC_TIMEBASE_FREQ);

    /* Timer sanity: measure ~100 ms busy-wait using mftb. */
    uint64_t t0 = mftb();
    tb_udelay(100 * 1000);
    uint64_t t1 = mftb();
    unsigned long measured_us = tb_diff_usec(t1, t0);
    int timer_ok = (measured_us >= 95000 && measured_us <= 110000);
    printf("[TIMER] busy-wait(100ms) measured=%lu us  (expect ~100000) %s\n",
           measured_us, timer_ok ? "PASS" : "FAIL");

    /* SMC sensors. Format: 4x uint16_t. Empirically CPU/GPU/eDRAM/mobo temps
     * in units of 0.25 °C (Q14.2 fixed point in the low bits per xenon_smc.c). */
    uint16_t s[4] = {0};
    xenon_smc_query_sensors(s);
    printf("[SMC] sensors raw: 0x%04x 0x%04x 0x%04x 0x%04x\n",
           s[0], s[1], s[2], s[3]);
    printf("[SMC] approx temps CPU=%u.%02u GPU=%u.%02u eDRAM=%u.%02u MB=%u.%02u (deg C)\n",
           s[0] >> 8, ((s[0] & 0xFF) * 100) / 256,
           s[1] >> 8, ((s[1] & 0xFF) * 100) / 256,
           s[2] >> 8, ((s[2] & 0xFF) * 100) / 256,
           s[3] >> 8, ((s[3] & 0xFF) * 100) / 256);

    int avpack = xenon_smc_read_avpack();
    printf("[SMC] AV pack code = 0x%02x\n", avpack);

    /* Network: lwIP init + DHCP. */
    printf("[NET] initializing lwIP...\n");
    int net_rc = network_init();
    printf("[NET] network_init returned %d\n", net_rc);

    /* Poll for DHCP lease up to ~15 s. */
    int have_ip = 0;
    for (int i = 0; i < 30; i++) {
        network_poll();
        if (netif.ip_addr.addr != 0) {
            have_ip = 1;
            break;
        }
        tb_udelay(500 * 1000);
    }
    if (have_ip) {
        uint32_t ip = netif.ip_addr.addr;   /* lwIP stores network-byte-order */
        uint32_t nm = netif.netmask.addr;
        uint32_t gw = netif.gw.addr;
        printf("[NET] DHCP OK  ip=%u.%u.%u.%u  nm=%u.%u.%u.%u  gw=%u.%u.%u.%u\n",
               (ip >>  0) & 0xFF, (ip >>  8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
               (nm >>  0) & 0xFF, (nm >>  8) & 0xFF, (nm >> 16) & 0xFF, (nm >> 24) & 0xFF,
               (gw >>  0) & 0xFF, (gw >>  8) & 0xFF, (gw >> 16) & 0xFF, (gw >> 24) & 0xFF);
        printf("[NET] status: PASS\n");
    } else {
        printf("[NET] no DHCP lease after 15s\n");
        printf("[NET] status: FAIL (check LAN cable + router DHCP)\n");
    }

    /* Controller: probe 4 ports for 3 s. */
    printf("[INPUT] polling controllers for 3s (press any button)...\n");
    int pad_seen = 0;
    for (int i = 0; i < 30; i++) {
        struct controller_data_s pad;
        for (int p = 0; p < 4; p++) {
            if (get_controller_data(&pad, p)) {
                if (pad.a || pad.b || pad.x || pad.y || pad.start || pad.back
                    || pad.up || pad.down || pad.left || pad.right
                    || pad.lb || pad.rb || pad.lt || pad.rt) {
                    printf("[INPUT] pad %d: A=%d B=%d X=%d Y=%d s1=(%d,%d) lt=%u rt=%u\n",
                           p, pad.a, pad.b, pad.x, pad.y,
                           pad.s1_x, pad.s1_y, pad.lt, pad.rt);
                    pad_seen = 1;
                }
            }
        }
        if (pad_seen) break;
        tb_udelay(100 * 1000);
    }
    printf("[INPUT] status: %s\n", pad_seen ? "PASS (button seen)" : "no button pressed in 3s");

    printf("========================================\n");
    printf(" hwtest complete.\n");
    printf(" Photograph the screen for the log.\n");
    printf(" Press BACK to reboot to XeLL.\n");
    printf("========================================\n");

    /* Idle loop — wait for BACK. */
    for (;;) {
        struct controller_data_s pad;
        for (int p = 0; p < 4; p++) {
            if (get_controller_data(&pad, p) && pad.back) {
                printf("[main] BACK pressed. Exiting.\n");
                return 0;
            }
        }
        tb_udelay(50 * 1000);
    }
}
