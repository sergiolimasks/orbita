# TEST P1 — hwtest (first boot on hardware)

## Objective

Confirm that the libxenon toolchain produces an ELF32 that XeLL can boot on
the actual Xbox 360 RGH, and that we can read: CPU/timebase, SMC temps, LAN
DHCP lease, controller input.

## Artifact

- Local path:  `app/hwtest/hwtest.elf32`
- Size:        <recorded on build>
- SHA-256:     <recorded on build>
- Built with:  xenon-gcc 9.2.0 + libxenon (see CHANGELOG)

## Deploy — pick ONE

### Option A — USB pendrive (simplest, no network dependency)

1. Plug a FAT32 or exFAT USB pendrive into this dev machine.
2. Run: `scripts/deploy-usb app/hwtest/hwtest.elf32`
   The script auto-finds the USB and copies the ELF as `xenon.elf` on the root.
3. Safely eject.
4. Plug pendrive into the Xbox 360 USB port.
5. Boot to XeLL:
   - either hold Eject on power-on (if XeLL is the primary boot),
   - or from Aurora/FSD dashboard, use "Launch XeLL" / plugin equivalent.
6. XeLL scans USB and auto-loads `xenon.elf` on first FAT partition.

### Option B — HTTP netboot via XeLL Reloaded (fastest iteration)

Requires XeLL Reloaded (0.994+) on the console, and the console on the same LAN as this dev machine.

1. `make -C app/hwtest` on the dev machine.
2. `scripts/serve-xell` — prints its LAN IPs and the URL to configure.
3. In XeLL menu on the console, set network boot URL to:
     `http://<dev-machine-ip>:8032/build/xenon.elf`
4. Trigger "Boot from network" in XeLL.
5. XeLL pulls the ELF and jumps to entry.

### Option C — FTP into HDD, then XeLL loader from HDD

If the console runs Aurora/FSD (has FTP), and there is a XeLL loader that
scans HDD paths:

1. `scripts/deploy-xbox app/hwtest/hwtest.elf32 hwtest.elf`
   uploads to `/Hdd1/Apps/XCloud360/hwtest.elf`.
2. Configure XeLL/loader to boot that path. **Xbox-side setup depends on
   installed loader** — Sergio to confirm which is available.

## Expected output on TV / HDMI

```
========================================
 XCloud360 hardware test (P1)
 build:    0.0.1-p1
 built:    <date> <time>
========================================
[CPU] xenon PPC64 BE, 3 cores / 6 hw threads
[CPU] main thread PIR=<0..5> (hw_thread=<0|1>, core=<0..2>)
[CPU] PPC_TIMEBASE_FREQ=49875000 Hz
[TIMER] busy-wait(100ms) measured=~100000 us  (expect ~100000) PASS
[SMC] sensors raw: 0xXXXX 0xXXXX 0xXXXX 0xXXXX
[SMC] approx temps CPU=~50.00 GPU=~50.00 eDRAM=~40.00 MB=~40.00 (deg C)
[SMC] AV pack code = 0x??
[NET] initializing lwIP...
[NET] network_init returned 0
[NET] DHCP OK  ip=192.168.15.??  nm=255.255.255.0  gw=192.168.15.1
[NET] status: PASS
[INPUT] polling controllers for 3s (press any button)...
[INPUT] pad 0: A=<0|1> B=<0|1> ... s1=(x,y) lt=? rt=?
[INPUT] status: PASS (button seen)
========================================
 hwtest complete.
 Photograph the screen for the log.
 Press BACK to reboot to XeLL.
========================================
```

## User actions

1. Press any button on the controller during the "polling controllers" window (3s).
2. When you see "hwtest complete", **photograph the entire screen**.
3. Press **BACK** to exit (returns to XeLL / reboots).

## Duration

~30 seconds active, plus your reading time.

## Pass / fail matrix

| Line | PASS if | FAIL means |
|---|---|---|
| CPU PIR | 0..5 | assembly broken or non-Xenon CPU |
| TIMER measured | 95000..110000 us | timebase constant wrong or busy-wait broken |
| SMC sensors | non-zero, plausible temps | SMC init broken |
| NET DHCP | valid IP in 192.168.15.x | network/router issue (or console offline) |
| INPUT | button register lights up | controller driver / USB stack broken |

## What to send back

Photograph or dictate the entire screen contents. Also confirm which deploy
option (A/B/C) worked. Any error message dumped by XeLL before jump also
matters (photograph the XeLL screen too if it errored).

## Next step

On PASS across the board → advance to P2 (threading benchmark, FTP log
retrieval). On any FAIL → we iterate on that specific subsystem before
advancing.
