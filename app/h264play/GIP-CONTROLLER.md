# h264play: wired Xbox One / Series controller support (GIP)

XCloud360 reads **wired** Xbox One, One S, Elite, Elite 2 and Series X|S controllers over USB. Most third-party "Designed for Xbox" pads (PowerA, PDP, Hori, …) should work too. All of it is a libxenon patch: the app needs **no source change**. A One pad shows up as an ordinary libxenon controller (`get_controller_data()`), so `poll_and_send_gamepad`, `apply_deadzone`, `apply_curve` and `menu_read_buttons` behave exactly as they do with a 360 pad.

**Status: WORKS ON HARDWARE.** The Series X|S pad (`045e:0b12`) plays fine on the console with v0.18.2. Since 2026-10-25 the patch is **installed as the toolchain's libxenon** (`~/devkitxenon`), so every normal `make` of the app gets wired One/Series pad support. Production build: `variants/v0182-prod.elf32`.

### Root cause (what was actually wrong)

1. **Cached OHCI descriptors (libxenon bug).** libxenon's page table (`drivers/ppc/vm.c`) maps RAM cached at `0x80000000` and cache-inhibited at `0xA0000000`. `ohci.c` meant to put its ED/TD/HCCA pools in uncached memory, but under `_CFE_` the `UNCADDR(x)` macro is `x|0x80000000`, the **cached** alias. The Xenon OHCI DMA does not snoop the CPU cache. CPU writes to descriptors (queueing a TD, TailP, the link that inserts an ED into the periodic list) could stay in L2 while the HC read stale memory. On hardware: the pad's IN ED was linked (the CPU's own walk reached it from 8 slots) but the HC never executed it (CC=F NotAccessed forever, even against an unconfigured device where the OUT got CC=5). Wired 360 pads mostly worked by luck, because cache lines get evicted. The same effect also explains the earlier "USB polling stops" stall: a synchronous control request whose TD the HC never saw makes `usb_wait_request()` spin forever. Fix: pools on `phys|0xA0000000`, `PHYSADDR = x & 0x1FFFFFFF`, pools rounded to 128-byte cache lines (`ohci.c`, local `#undef`s; nothing else in libxenon uses these macros).
2. **The RF unit holding slot 0 (app side, fixed in main.c by the coordinator).** The internal 360 wireless receiver (`045e:02aa`) takes controller slot 0, so the One pad landed on slot 1, which the v0.17/0.18 menu never read. v0.18.2 merges all pads for the menu/Back+Start and maps the xCloud player index by first use.

Everything else that changed during the bisect turned out not to be the cause: the XboxInput-style handshake, the 4 ms interval placement, the alt settings, the IN sizes. It is kept because it matches the working retail plugin and Windows. The v4/v4.1 red screens were XeLL itself, not our code.

| what | where |
|---|---|
| libxenon patch | `app/h264play/gip/libxenon-gip.patch` (`patch -p1` in `~/libxenon/libxenon`) |
| host test | `app/h264play/gip/hosttest/` (`run.sh <patched libxenon root>`) |
| test ELF | `variants/gip.elf32` (+ `.sha256`), VERSION `0.17.1-gip` |
| build tree used | patched libxenon in `~/libxenon-gip-src`, installed in its own prefix `~/libxenon-gip`. The shared `~/devkitxenon` was not touched. |

## Design

New files in `libxenon/drivers/usb/`:

- `gip_proto.c/.h`: the protocol only, with no USB, no malloc and no globals. It contains the header/varint decoder, the packet builders (power-on, identify, locale, LED, acks including chunk acks, rumble, the xpad quirk list) and the parser (report 0x20, guide 0x07, announce, identify chunks, status). One USB transfer may carry several GIP packets.
- `usbgip.c`: the USB glue (`usbgip_driver`), which has two start-up modes (below).
- `usblog.c/.h`: diagnostics that can never block (see "Diagnostics").

Edits to existing files:

- `usbdevs.c`: PID entries for Microsoft pads, plus a class fallback: a device or interface 0 with class `FF/47/D0` gets `usbgip_driver`. The Xbox Wireless Adapter PIDs are pinned to `dummy_driver`. 360 pads still match first, unchanged. Every enumeration is logged.
- `usbctrl.c`: exports `usbctrl_slot_alloc()/usbctrl_slot_free()` on the existing `controller_mask`, so 360 and One pads share the same 4 slots with the same "lowest free index" rule.
- `usbd.c/.h` and `ohci.c`: new `usb_open_pipe_interval()`. An interrupt pipe opened through it is polled at its `bInterval` (rounded down to 1/2/4/8/16/32 ms). libxenon normally hangs **every** interrupt ED on one 32 ms leaf of the OHCI tree, while the retail USB stack polls these pads at the descriptor's 4 ms. Only GIP uses the new call, so 360 pads and the RF module keep their current behaviour. The chosen tree nodes (0, 1, 3, 7, 15, 31) lie on the ancestor chain of `inttable[0]`, so the unchanged `ohci_ept_delete()` still finds and removes the ED.
- `usbhub.c`, `usbmain.c`, `tinyehci/ehci.c`, `tinyehci/usbstorage.c`: log hooks only. `usb_do_poll()` also calls `usbgip_tick()` and `usblog_tick()`.
- `input.c`: `set_controller_rumble()` falls back to `usbgip_set_rumble()`.
- `ports/xenon/Makefile`: adds `usbgip.o gip_proto.o usblog.o`.

### Start-up, Microsoft pads (VID 045e): "host" mode, as XboxInput / Windows do it

This mode is taken from XboxInput (`controller_backend.cpp` profile for `0x0B12`: `POWER_BEFORE_ANNOUNCE | IDENTIFY_ACK | GUIDE_SEPARATE`, endpoints `82/02`, interval 4, 64 bytes) and from the Windows capture in riffmaster-rgh360 `docs/PROTOCOL.md` §5.

1. `SET_CONFIGURATION 1`. **No `SET_INTERFACE`** (XboxInput only sends it for one PowerA model).
2. Open interrupt OUT `0x02`, then IN `0x82` from interface 0 **alt 0**, both at `bInterval` (4 ms). xpad, xone and XboxInput all use alt 0 and never select alt 1.
3. Early power-on `05 20 00 01 00` (sequence 0, byte for byte XboxInput's). **The first IN read is queued only when it completes**, the same order XboxInput uses. If it hasn't completed after 1 s, the read starts anyway. `02d1` (2013 pad) skips the early power-on and reads first, as in XboxInput.
4. The pad sends ANNOUNCE (`02 20 ss 1c` + VID/PID at payload 8–11) → the host sends **IDENTIFY** `04 20 ss 00`. The pad keeps announcing until it sees IDENTIFY, so IDENTIFY is re-sent at most once a second while announces continue and no identify reply has arrived.
5. The pad replies with the identify descriptor in chunks (`04 f0 02 3a eb 01 …`, 235 bytes). Each chunk that asks for one gets an ack laid out as xone's `gip_pkt_acknowledge`, e.g. `01 20 02 09 00 04 20 3a 00 00 00 b1 00`. These are the bytes XboxInput captured.
6. On the last (zero-length) chunk the host sends the 15-byte locale POWER `05 20 ss 0f 06 00 00 00 00 00 00 55 53 00 00 00 00 00 00`, then power-on `05 20 ss 01 00`, then LED `0a 20 ss 03 00 01 14`. The driver logs `init ok`. No auth: XboxInput sends none for this pad on this console.
7. Input (`0x20`) and Guide (`0x07`, acked) follow. A new announce after power-up means the pad rebooted its GIP side, so the handshake restarts.
8. If nothing at all is received 3 s after the power-on, the early power-on is re-sent (up to 5 times).

### Start-up, third-party pads: "xpad" mode

This is the Linux xpad sequence, unchanged from v1: `SET_INTERFACE(1, alt 0)`, read, then xpad's `xboxone_init_packets` for the PID (power-on, LED, auth-done, plus quirks: identify-ack for PDP `0e6f:0165` and Hori `0f0d:0067`, rumble kick for PowerA `24c6:54xa`), re-sent on every announce and on a 3 s retry timer.

### Input mapping

- Report `0x20` (≥ 18 bytes) is decoded into `controller_data_s` with the 360 driver's ranges. Sticks are raw s16 with the same "Y positive = up" sign as the raw 360 report (xpad flips both for Linux), so there is **no sign change** and `menu_read_buttons`' `s1_y > 20000 == up` stays correct. Triggers are 10 bit, `>>2` gives 0..255. Menu → `start`, View → `back`, LB/RB, stick clicks, D-pad, A/B/X/Y.
- `0x07` sets `logo` (the field the 360 Guide uses).
- The state is latched like the 360 driver's. Unplug publishes an all-zero state (no stuck stick or button).

### Memory and threading

Allocation happens only in attach: the softc, two 64-byte DMA buffers (128-byte aligned, so each sits on its own Xenon cache line), one IN and one OUT usbreq, all freed in detach. Requests are reused, and outgoing packets go through a fixed 16-entry ring. **Nothing is allocated per packet.** The host test runs 100k reports with zero allocations and 1000 plug/unplug cycles that return the heap to its starting count. Everything runs inside `usb_do_poll()` (XCloud360: only the network thread).

### Diagnostics

- `usblog()` only formats into a static history: the first 96 lines since boot plus the last 48. It prints on the console during the first 6 s only, so there is no console scrolling on the network thread.
- Nothing is sent from USB callbacks. `usblog_tick()` (once per `usb_do_poll`) is the only caller of the app's `np_log_line()` (weak, never blocks). It sends **at most one line per 4 ms**:
  - a one-line summary every 2 s (every 30 s after 2 minutes)
  - otherwise the next history line
  - a rewind of the whole history at +8/15/25/40/60/90/180 s
- Summary format: `LOG [gip] sum t=… enumN: vid:pid>drv … | p0 045e:0b12 Hinit-ok idT/D rN rtN txOK/TX eLAST rxN inN anN ieN/LAST`. Here `H`/`X` is the mode and `idT/D` is identify tries / done.
- Logged per pad:
  - attach and all descriptors, `SET_CONFIGURATION`, the pipes with their intervals
  - every OUT packet in hex with its completion status
  - the first 16 IN packets in hex
  - announce VID/PID, `-> IDENTIFY`, `identify complete`, `init ok`, `first input`
  - retries, IN errors, and a stats line every 10 s

How to read the next log (`grep "LOG \[gip\]"`):

| you see | meaning |
|---|---|
| `IN 0x82 read queued (early power-on done)` then `rx#1 … 02 20 …` | announce read: the IN path works (v1–v3 never got here) |
| `-> IDENTIFY`, then `rx# … 04 f0 …` and acks | handshake running |
| `identify complete …`, `init ok`, `first input` | working, and the LED should be solid |
| `nothing received … retry k/5` with `tx … ok` | the pad still never answers IN. Next suspect: OHCI interrupt-IN scheduling itself |

## v4.1: red screen at boot, bisect builds

v4 (`ef65831e…`) showed the libxenon red crash screen at boot with the Series pad plugged in, and nothing at all reached UDP. v4.1 adds no fix, only isolation:

| ELF | GIP interrupt polling | everything else |
|---|---|---|
| `variants/gip.elf32` (VERSION `0.17.1-gip`) | `usb_open_pipe_interval`, 4 ms (tree node 3) | v4 handshake |
| `variants/gip-slot32.elf32` (VERSION `0.17.1-gip-slot32`) | stock `usb_open_pipe`, 32 ms leaf (`GIP_OHCI_INTERVAL=0`) | identical |

- If only `gip.elf32` crashes, the interval placement is to blame. If both crash, it is the handshake/input path (the first time IN data really flows on hardware).
- **The crash screen now names the step.** libxenon's `crashdump()` (`drivers/ppc/c_except.c`) clears the console, so on-screen breadcrumbs would be erased. The patch adds a weak hook: under the registers and stack, the red screen prints **the last 12 USB/GIP log lines** ("last USB/GIP log lines (oldest first)"). These are static buffers and string copies only, safe in the exception handler.
- Breadcrumb lines (`bc …`) bracket every risky step and are also forced onto the console:
  - attach entry, endpoints found, softc/DMA buffer addresses, `SET_CONFIGURATION`
  - before/after each pipe open, and inside `ohci_ept_create`: ED address, tree node, link result
  - request creation, the early power-on, the first IN queue
  - the first 8 OUT queues and their completions
  - the first 8 IN completions: entry, parsed, re-queued
  - IDENTIFY, identify complete
- **Decoding the photo:** the screen shows `sr0` (= PC) and `lr`. Map them with the unstripped ELFs kept next to the variants, e.g. `xenon-addr2line -f -e variants/gip.elf 0x8022b2d8` (or `variants/gip-slot32.elf`).

### OHCI model test

`gip/ohcitest/test_ohci.c` compiles the **real patched `ohci.c`** on Linux. The memory pools live in an arena mapped twice (at `A` and `A|0x80000000`), so libxenon's `PHYSADDR`/`UNCADDR` addressing works as on the Xbox, and register reads advance a fake frame counter so `ohci_ept_delete()` runs for real. It walks the HCCA interrupt table the way the HC does. The endpoint set comes from the v3 log: RF module `02aa` (8 EDs, stock placement) plus `0b12` OUT `0x02` and IN `0x82` at interval 4. Checks (80):

- no cycles from any of the 32 slots
- 4 ms EDs visited from exactly 8 slots, stock EDs from 1, other intervals at the right depth
- ED control word (FA/EN/MPS/dir/no skip)
- an IN TD queued on the 4 ms ED points at the DMA buffer
- delete with a pending TD completes it as cancelled
- 500 create/delete cycles mixed with RF churn, no ED leaked

It passes, so the placement logic itself looks sound. A hardware-only cause (HC timing) is still possible, and that is what the bisect is for.

```sh
D=/mnt/storage/projetos/PROJETOS/XCloud360/app/h264play/gip; S=<patched libxenon>/drivers
gcc -std=gnu99 -no-pie -fno-pie -D_CFE_=1 -DGIP_HOSTTEST=1 -I $D/ohcitest -I $S -I $S/usb -I $S/nocfe -I $S/../include \
    $D/ohcitest/test_ohci.c $S/nocfe/lib_queue.c $S/usb/usblog.c -o /tmp/test_ohci && /tmp/test_ohci | grep "ohci model"
```

## Production vs diagnostic build

`GIP_DEBUG` (in `drivers/usb/usblog.h`, default **0**) selects the build. The installed libxenon is the production one.

| | production (`GIP_DEBUG=0`, installed) | diagnostic (`GIP_DEBUG=1`) |
|---|---|---|
| start-up | always `base` (v4.2 handshake) | cycles one bisect variant per plug-in |
| UDP log | enum, attach, `connected`, `init ok`, `first input`, retries/errors, `detached`; `[gip] sum#N … hbM …` every **30 s** | + descriptors, every OUT/IN in hex, per-pad stats, OHCI ED/TD/HC dumps every 250 ms, boot replay, summary every 2 s |
| TV console | only during the first 6 s of boot | same, plus forced breadcrumbs |
| breadcrumbs (`bc …`) | crash-screen ring only | console + UDP + crash ring |
| crash screen | GIP state + last 10 log lines printed first | same |

To build a diagnostic libxenon into a side prefix (never over the installed one): `make -C ports/xenon libxenon.a CC="xenon-gcc -DGIP_DEBUG=1"` in a copy of the tree, then `make install DEVKITXENON=$HOME/libxenon-gip`.

Host tests: `gip/hosttest/run_all.sh <libxenon root>` runs the full suite as a diagnostic build (1219 checks) and a production build (128 checks). `gip/ohcitest/test_ohci.c` (88 checks) runs the real `ohci.c`.

### Installed (2026-10-25)

- `~/libxenon/libxenon` patched with `gip/libxenon-gip.patch`, rebuilt (`tinyehci.o` removed first) and installed with `make install`.
- Backups:
  - `~/devkitxenon/usr/lib/libxenon.a.pre-gip` (`b6a28d49…`)
  - `~/devkitxenon/pre-gip-backup-20261025-0322.tgz` (`usr/include`, `libxenon.a`, crt objects, `app.lds`, `rules`)
  - `~/libxenon/libxenon-src-pre-gip-20261025-0322.tgz` (`drivers/`, `ports/xenon/Makefile`)
- New `libxenon.a`: `a51ae4e2…`.
- Rollback:
  ```sh
  cd ~/libxenon/libxenon && patch -R -p1 < …/gip/libxenon-gip.patch
  tar xzf ~/devkitxenon/pre-gip-backup-20261025-0322.tgz -C ~/devkitxenon
  ```

## Supported devices

Microsoft (host mode): `02d1` Xbox One, `02dd` One (2015 fw), `02e3` Elite, `02ea` One S, `0b00` Elite 2, `0b0a` Adaptive, `0b12` Series X|S. Anything else with `FF/47/D0` (PowerA, PDP, Hori, Razer, 8BitDo, Turtle Beach …) runs in xpad mode with xpad's per-PID quirks.

Not supported: wireless (the Xbox Wireless Adapter is a WiFi dongle and stays on `dummy_driver`), Bluetooth, and inputs `controller_data_s` has no field for (Share, paddles, profile).

## Hardware check

1. Boot `gip.elf32` with the pad plugged in. Within a second or two the LED should go solid.
2. Press buttons in the menu. The PC log should show `first input`. D-pad and left stick navigate (stick up = list up), A selects.
3. Hot-plug: unplug, then replug, and again after the menu is up.
4. In a game: sticks (camera Y like the 360 pad), triggers 0–full, Guide, Back+Start for 2 s back to the menu.
5. With a 360 pad also plugged in, the first pad gets slot 0. **The app reads slot 0 only** (unchanged behaviour).

## Risks and open points

- The v4 root-cause fix is unproven until the next hardware run. Two things changed together: the start-up order/bytes (now XboxInput's) and the 4 ms interrupt polling. If it works, either could be the one that mattered.
- `usb_open_pipe_interval()` changes OHCI scheduling for GIP pipes only. Polling at 4 ms instead of 32 ms costs a few more periodic-list visits; the RF module and 360 pads are unaffected.
- Elite paddles, Share and the Adaptive profile button are dropped.
- `usbgip_set_rumble()` must be called from the thread that runs `usb_do_poll()`, the same contract as `usbctrl_set_rumble()`. The app does not use rumble today.

## Merge into the main tree (audio + GIP)

The patch is **libxenon-only**. The app (including the v0.18.0 audio build) needs no change, so the final build is just the audio tree linked against the patched libxenon:

```sh
cp ~/devkitxenon/usr/lib/libxenon.a ~/devkitxenon/usr/lib/libxenon.a.pre-gip     # backup
cd ~/libxenon/libxenon
patch -p1 < /mnt/storage/projetos/PROJETOS/XCloud360/app/h264play/gip/libxenon-gip.patch
sh /mnt/storage/projetos/PROJETOS/XCloud360/app/h264play/gip/hosttest/run.sh ~/libxenon/libxenon   # 1187 checks, 0 failures
source /mnt/storage/projetos/PROJETOS/XCloud360/scripts/env.sh
rm -f drivers/usb/tinyehci/tinyehci.o      # no header deps; tinyehci.c #includes ehci.c/usbstorage.c
cd ports/xenon && make libxenon.a && make install   # with no other xenon build running
cd /mnt/storage/projetos/PROJETOS/XCloud360/app/h264play && make clean && make
python3 ~/ffmpeg-xenon-mt-altivec/xcloud360/vmxaudit.py h264play.elf32   # 0 hits
```

`make install` re-copies headers, `app.lds`, `rules` and crt objects into `~/devkitxenon`, as the toolchain script does. Rebuilt objects: `input.o c_except.o usbctrl.o usbdevs.o usbd.o ohci.o usbhub.o usbmain.o tinyehci.o` plus the new `usbgip.o gip_proto.o usblog.o`. Pristine rebuilds of the touched objects were checked to disassemble identically to the installed ones, so the diff is only the patch. Rollback: restore `libxenon.a.pre-gip` (and `patch -R -p1`).

Without installing: `make LIBXENON_LIB=$HOME/libxenon-gip/usr/lib LIBXENON_INC=$HOME/libxenon-gip/usr/include` (command-line variables override `$(DEVKITXENON)/rules`). This is how `variants/gip.elf32` was built.

## Licensing / provenance

| part | derived from | license |
|---|---|---|
| report 0x20 layout, guide ack bytes, rumble layout, xpad-mode init list + quirk table (`gip_proto.c`), SET_INTERFACE(1,0) | Linux `drivers/input/joystick/xpad.c` | GPL-2.0-or-later |
| GIP header / LEB128 framing, chunk-ack layout, "sequence never 0" (`gip_proto.c`) | medusalix/xone `bus/protocol.c` | GPL-2.0-or-later |
| host-mode order for Microsoft pads (early power-on with sequence 0 then read, IDENTIFY on announce + 1 s re-send, ack identify chunks, then locale POWER, power-on, LED; no SET_INTERFACE, no auth), the 15-byte locale packet, the endpoint/interval profile for `0b12` (`usbgip.c`, `gip_proto.c`) | bloxmaster0811/XboxInput `source/main.cpp`, `controller_backend.cpp` | **GPL-3.0** |
| Windows capture of the same sequence and its bytes (IDENTIFY `04 20 ss 00`, chunked reply, locale POWER) | Durg5/riffmaster-rgh360 `docs/PROTOCOL.md` | **GPL-3.0** |
| OHCI interval placement, `usb_open_pipe_interval`, usblog, slot sharing, the libxenon glue | new code on libxenon (Broadcom CFE licence for the files it edits) | — |

No code was copied verbatim from XboxInput or riffmaster. They are C++ against the retail kernel's `Usbd*` API, while this is C on libxenon OHCI. But the v4 handshake **follows their sequence and bytes closely**, so treat the patched libxenon as **GPL-3.0-compatible derived work** (xpad and xone are "or-later", so the combination can be GPL-3.0). The riffmaster-rgh360-fixed changes (RSA self-test, HID pass-through) concern the retail kernel's auth/HID path and were not needed here. A binary built with the patched libxenon carries GPL obligations, like the `GPU=1` xmplayer shaders already do. The repo has no top-level license yet; that's the user's call before publishing.

Suggested THIRD_PARTY.md rows:

| xpad (Xbox One part) + xone | torvalds/linux `drivers/input/joystick/xpad.c`, medusalix/xone `bus/protocol.c` | GPL-2.0-or-later | `app/h264play/gip/libxenon-gip.patch` → libxenon `drivers/usb/gip_proto.c` | Close port of report/ack/rumble layouts, xpad init quirks, GIP framing. |
| XboxInput + riffmaster-rgh360 (protocol notes) | bloxmaster0811/XboxInput, Durg5/riffmaster-rgh360 `docs/PROTOCOL.md` | GPL-3.0 | same patch, `usbgip.c` host mode | Start-up sequence and bytes for Microsoft pads on an Xbox 360 USB host, followed closely (no verbatim code). |

## History

- v1: xpad sequence at attach. Untested on hardware.
- v2: diagnostics; the replay ran inside `usb_do_poll` and sat behind the login's USB hold.
- v3: non-blocking paced logging. On hardware: `0b12` enumerates, `SET_CONFIGURATION`/`SET_INTERFACE` ok, 18/18 OUT ok, **0 IN completions**, LED blinking, self-reset at about 31 s.
- v4: XboxInput's sequence (no SET_INTERFACE, no early LED/auth, sequence-0 power-on before the first read, IDENTIFY/chunk-ack/locale/power/LED), 4 ms interrupt polling via `usb_open_pipe_interval`, multi-packet transfers, 128-byte-aligned DMA buffers.
- v4.1: crashed at boot (red screen). Added bisect builds (`gip` 4 ms / `gip-slot32` stock), breadcrumbs, the crash-screen log tail and the OHCI model test. No behaviour change otherwise.
- v4.2 retest: the v4/v4.1 red screens were XeLL itself (identical crash with stock-libxenon v0.18.1); those builds most likely never ran. `variants/gip.elf32` = v0.18.1 (audio + multipad, unchanged sources) linked against v4.1 + the v4.2 hardening (`gip/libxenon-gip-v42-hardening.patch`, now also folded into `gip/libxenon-gip.patch`): DoneHead pointer mask, refuse transfers instead of NULL TDs, IN/OUT retry after a refusal, TD low-water in the summary, crash screen prints the GIP state and last log lines first. 4 ms interval placement on. `variants/gip-slot32.elf32` is stale (v0.17.1 base).
- v5 (bisect build, `variants/gip.elf32`, v0.18.1 base): v4.2 failed the same way (OUT 0x02 ACKed, IN 0x82 never completes, LED blinking, self-disconnect ~15 s). v5 cycles one variant per plug-in (no-setcfg, setif0.0, alt1, in32, in512, 2in, seq1; shown as `v=` in every summary and dump) and, for the first 60 s after each attach, dumps the raw OHCI state every 250 ms: IN ED dword0/HeadP/TailP/NextED, vis = HCCA slots that reach the ED, the head TD (CC/EC/T/DI/R, CBP/BE); every 1 s also HcControl/FmNumber/PeriodCurrentED/PeriodicStart/IntStatus/IntEnable/HCCA[0]/DoneHead/root-hub ports, the OUT ED, and the 360 RF unit IN ED + its completion count (rfrx). Watchdog print sign bug fixed.
- v5.1: ROOT-CAUSE CANDIDATE FIXED IN LIBXENON OHCI. libxenon maps RAM cached at 0x80000000 and cache-inhibited at 0xA0000000 (drivers/ppc/vm.c), but ohci.c (via cfe.h UNCADDR = x|0x80000000) put its ED/TD/HCCA pools on the CACHED alias while the OHCI DMA does not snoop the CPU cache. CPU writes to descriptors (TD queueing, TailP, the ed_next_ed link that inserts an ED into the periodic list) could stay in L2 and the HC kept reading stale memory: v5 showed the IN ED linked (CPU walk vis8) but never executed (CC=F, even against an unconfigured device where OUT got CC=5). Fix: pools on phys|0xA0000000, VTOP = x & 0x1FFFFFFF, pools rounded to 128-byte lines. Likely also explains the v5 silence after plug #2 (a synchronous SET_INTERFACE whose control TD the HC never saw: usb_wait_request spins forever on the network thread). Logger: sum#N + hb heartbeat. Variant cycle: base, setif0.0, alt1, in32, in512, 2in, seq1.
- v5.2 / production: `GIP_DEBUG` switch (production default), installed into `~/devkitxenon`, shared tree rebuilt as v0.18.2 → `variants/v0182-prod.elf32`. Works on hardware.
