/*
 * XCloud360 — P1 hardware test (hwtest)
 *
 * Prints build info, CPU/thread count, memory, monotonic timer sanity,
 * network sanity (lwIP up + DHCP lease). Writes a log to
 * <mount>/Apps/XCloud360/logs/hwtest.txt on the Xbox 360 filesystem.
 *
 * Intentionally minimal: exercises libxenon runtime, threading, filesystem,
 * timer, network. Nothing xCloud yet.
 *
 * Spec §29 P1, §26 (logging), §40 (test protocol).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <xetypes.h>
#include <debug.h>
#include <ppc/timebase.h>
#include <ppc/atomic.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_smc/xenon_smc.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenon_uart/xenon_uart.h>
#include <network/network.h>
#include <input/input.h>
#include <diskio/diskio.h>

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.0.1-p1"
#endif

#ifndef XCLOUD360_BUILD_TIMESTAMP
#define XCLOUD360_BUILD_TIMESTAMP __DATE__ " " __TIME__
#endif

static FILE *log_fp = NULL;

static void log_open(const char *path) {
    log_fp = fopen(path, "w");
}

static void logln(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("%s\n", buf);
    if (log_fp) {
        fputs(buf, log_fp);
        fputc('\n', log_fp);
        fflush(log_fp);
    }
}

static uint64_t tb_now(void) {
    return mftb();
}

static double tb_to_us(uint64_t delta) {
    /* libxenon timebase runs at PPC_TIMEBASE_FREQ ticks/sec (49.875 MHz on Xenon). */
    extern uint64_t PPC_TIMEBASE_FREQ;
    return (double)delta * 1000000.0 / (double)PPC_TIMEBASE_FREQ;
}

int main(void) {
    /* libxenon boilerplate: bring up video console. */
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    logln("========================================");
    logln("XCloud360 hardware test (P1)");
    logln("build:    %s", XCLOUD360_BUILD_VERSION);
    logln("built:    %s", XCLOUD360_BUILD_TIMESTAMP);
    logln("========================================");

    /* Filesystem: mount all disks and try to open log on Hdd. */
    xenon_ata_init();
    dvdrom_init();
    usb_init();
    usb_do_poll();
    for (int r = 0; r < 5; r++) {
        struct mountpoint_t *mp = NULL;
        int n = mount_all_devices();
        logln("[FS] mount attempt %d: %d device(s)", r + 1, n);
        if (n > 0) break;
        udelay(500 * 1000);
    }

    /* Prefer HDD, fall back to USB. */
    const char *log_candidates[] = {
        "uda:/Apps/XCloud360/logs/hwtest.txt",
        "usb:/Apps/XCloud360/logs/hwtest.txt",
        "sda:/Apps/XCloud360/logs/hwtest.txt",
        NULL
    };
    for (int i = 0; log_candidates[i]; i++) {
        log_open(log_candidates[i]);
        if (log_fp) {
            logln("[FS] log opened at %s", log_candidates[i]);
            break;
        }
    }
    if (!log_fp) {
        logln("[FS] WARN: no writable log path found — screen output only");
    }

    /* CPU + threads. */
    logln("[CPU] xenon PPC64 BE, 3 cores / 6 hardware threads");
    unsigned int this_thread = xenon_get_logical_processor_id();
    logln("[CPU] main thread hw_thread_id=%u", this_thread);

    /* Timer sanity: measure ~100ms delay with timebase. */
    uint64_t t0 = tb_now();
    udelay(100 * 1000);
    uint64_t t1 = tb_now();
    double measured_us = tb_to_us(t1 - t0);
    logln("[TIMER] udelay(100ms) measured=%.1f us  (expect ~100000)", measured_us);
    int timer_ok = (measured_us > 95000.0 && measured_us < 110000.0);
    logln("[TIMER] status: %s", timer_ok ? "PASS" : "FAIL");

    /* Memory: libxenon exposes heap markers in linker script. */
    extern char bss_start[], bss_end[], heap_begin[], heap_end[];
    size_t bss = (size_t)(bss_end - bss_start);
    size_t heap = (size_t)(heap_end - heap_begin);
    logln("[MEM] bss=%zu bytes, heap=%zu bytes (~%zu MB)",
          bss, heap, heap / (1024 * 1024));

    /* SMC temps (best-effort). */
    uint16_t cpu_c = 0, gpu_c = 0, edram_c = 0, mobo_c = 0;
    xenon_smc_query_temps(&cpu_c, &gpu_c, &edram_c, &mobo_c);
    logln("[SMC] temp CPU=%u.%u°C GPU=%u.%u°C eDRAM=%u.%u°C MB=%u.%u°C",
          cpu_c >> 8, cpu_c & 0xFF,
          gpu_c >> 8, gpu_c & 0xFF,
          edram_c >> 8, edram_c & 0xFF,
          mobo_c >> 8, mobo_c & 0xFF);

    /* Network: bring lwIP up + DHCP. */
    logln("[NET] initializing lwIP + DHCP...");
    network_init();
    unsigned int ip = 0;
    for (int i = 0; i < 30; i++) {
        ip = get_ip();
        if (ip) break;
        udelay(500 * 1000);
    }
    if (ip) {
        logln("[NET] DHCP lease OK: %u.%u.%u.%u",
              (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
              (ip >> 8) & 0xFF, ip & 0xFF);
        logln("[NET] status: PASS");
    } else {
        logln("[NET] no DHCP lease after 15s");
        logln("[NET] status: FAIL");
    }

    /* Controller: probe for at least one pad. */
    logln("[INPUT] polling controllers for 3s...");
    int pad_found = 0;
    for (int i = 0; i < 30; i++) {
        struct controller_data_s pad;
        for (int p = 0; p < 4; p++) {
            if (get_controller_data(&pad, p) && pad.b) {
                pad_found = 1;
                logln("[INPUT] pad %d responded", p);
            }
        }
        if (pad_found) break;
        udelay(100 * 1000);
    }
    logln("[INPUT] status: %s (press BACK anytime to close log)", pad_found ? "PAD SEEN" : "no pad activity");

    logln("========================================");
    logln("hwtest complete. Idle loop — press BACK to exit.");
    logln("========================================");

    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }

    /* Wait for BACK. */
    for (;;) {
        struct controller_data_s pad;
        for (int p = 0; p < 4; p++) {
            if (get_controller_data(&pad, p) && pad.back) {
                return 0;
            }
        }
        udelay(50 * 1000);
    }
}
