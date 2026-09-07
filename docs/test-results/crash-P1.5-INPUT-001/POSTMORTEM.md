# P1.5-INPUT-001 — Machine Check on inputdiag.elf32

**Ticket:** P1.5-INPUT-001 = MACHINE_CHECK
**Date:** 2026-09-07
**Reporter:** Sergio (hardware run on Xbox 360 RGH via XeLL)
**Status:** ROOT CAUSE PARTIALLY IDENTIFIED. Redesign in progress (inputdiag-safe).

## Symptoms

Exception vector `0x200` (Machine Check) during execution of `inputdiag.elf32`, before any RF module enumeration data could be captured on screen.

### Registers captured

| Reg | Value | Meaning |
|---|---|---|
| PIR | `0` | HW thread 0 |
| DAR | `0x00000000c8000000` | Data Address Register — MMIO region, likely NAND (0xC800_0000 = physical NAND / SFC) |
| SRR0 | `0x0000000080016920` | PC at fault (imprecise on Machine Check) |
| SRR1 | `0x100000000020B3030` | MSR at fault (SF/64-bit set, misc state) |
| LR | `0x0000000080014e5c` | Link register |

### Stack

```
0x80016920 → 0x80014e5c → 0x80001f04 → 0x800028f4
```

## Symbolization (via `xenon-addr2line` on `inputdiag-crash.elf`)

| Address | Function | File:line |
|---|---|---|
| `0x80016920` | `_vfprintf_r` | `newlib-3.1.0/newlib/libc/stdio/vfprintf.c:1528` |
| `0x80014e5c` | `_vfprintf_r` | `newlib-3.1.0/newlib/libc/stdio/vfprintf.c:852` |
| `0x80001f04` | `__crtmain` | `libxenon/ports/xenon/../../startup/xenon/crt1.c:27` |
| `0x800028f4` | `b32` | `startup_from_xell.o` |

## Disassembly around SRR0

```
80016910:  81 21 00 78  lwz     r9,120(r1)
80016914:  7f 10 c3 78  mr      r16,r24
80016918:  81 41 00 74  lwz     r10,116(r1)
8001691c:  4b ff ff ac  b       800168c8 <_vfprintf_r+0x1ab8>
80016920:  7e 43 93 78  mr      r3,r18         ← SRR0 (imprecise)
80016924:  7d 3a 4b 78  mr      r26,r9
80016928:  3b 80 00 00  li      r28,0
8001692c:  4b ff ae 1d  bl      80011748 <strlen>
80016930:  7c 7f 18 f8  not     r31,r3
```

The instruction at SRR0 (`mr r3, r18`) is a register-register move and cannot itself fault. Machine Check on PowerPC is **imprecise** — SRR0 often points near, not at, the faulting instruction. The immediately following `bl <strlen>` at `0x8001692c` is the likely trigger: `_vfprintf_r` is processing a `%s` argument, moves the char* into r3, and calls `strlen` which reads from `0xC8000000` — MMIO region → Machine Check with DAR reflecting the load address.

## Analysis

Stack backtrace confirms the fault happened **inside `printf()` called from `main()` of inputdiag** (0x80001f04 = `__crtmain`, next frame down is main's printf).

Two possible upstream causes:

### Cause A (most likely) — unsafe manual iteration of USB internals

`inputdiag/source/main.c::dump_devices()` walks `usb_buses[b]->ub_devices[addr]` directly and dereferences `usbdev_t` internal fields (`ud_devdescr`, `ud_drv`). While libxenon's `ohci.c` does `memset(bus, 0, sizeof(usbbus_t))` at bus creation and `usb_delete_device()` clears slots to NULL, we made assumptions about:

- `usb_buscnt` being initialized before we read it,
- `usb_buses[]` being fully populated in a state we can observe from user code,
- device descriptor fields being safely readable at any poll boundary.

The libxenon example `devkitxenon/examples/xenon/file/browser/source/main.c` — which is known to work with USB controllers — **never** includes `<usb/usbd.h>`, never iterates `usb_buses[]`. It uses only the public API: `usb_init()`, `usb_do_poll()`, `get_controller_data()`.

### Cause B (contributory) — long printf with many %s arguments

The `dump_devices()` printf has two `%s` slots (`drvname`, `name ? name : "..."`). If either slot is fed a stale/dangling pointer (e.g. from a partially-formed `usbdev_t`), `_vfprintf_r` will call `strlen()` on garbage → walks into MMIO → Machine Check with DAR near `0xC8xxxxxx`.

## Decision (per user directive)

Do NOT patch speculatively. Redesign as **`inputdiag-safe`**:

1. Restrict to public libxenon USB API only (as browser example does).
2. `get_controller_data()` poll loop — same code path any working libxenon app uses.
3. No manual walk of `usb_buses[]` or `ub_devices[]`.
4. No printf format with more than 4 arguments.
5. Guard every printf with sanity: never `%s` a pointer derived from library internals.
6. Same 20 s window for the user to mash all buttons.
7. Same in-band SUMMARY / DIAGNOSIS, but produced from safe events only.

If `inputdiag-safe` still yields zero events with a wireless-RF-only pad, the finding is stronger than any manual enumeration would have been: **the standard usbctrl path does not see the wireless controller**, which is exactly what we needed to know for ADR-006's decision matrix (branch (c): push product to XEX/kernel for input parity, or use wired/receiver).

## Preserved artifacts (for post-mortem)

- `inputdiag-crash.elf` — unstripped 1.6 MiB ELF with all symbols
- `inputdiag-crash.elf32` — stripped 187 KiB (matches what ran on hardware)
- `inputdiag-crash.meta.txt` — full metadata from build-metadata
- `inputdiag-crash.sha256` — `921d800c28b7d758efd26c137b78ca736ebc8ba8dddf96052b4ba764b8d61a3f`
- `inputdiag.elf32.map` (if present in build dir)

## Related

- ADR-006 (wireless RF gap)
- Spec §41 (if a test fails: capture error → reduce → localize → reproduce → instrument → fix → repeat exactly the same test)
