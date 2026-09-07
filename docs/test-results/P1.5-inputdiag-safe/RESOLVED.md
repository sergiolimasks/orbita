# P1.5 inputdiag-safe — RESOLVED (usbctrl path works with internal RF)

**Test:** `inputdiag-safe.elf32` on hardware, 2026-09-07.
**Reporter:** Sergio, 4 photos of the TV.
**Result:** PASS — wireless pad paired to the INTERNAL RF module IS visible to libxenon's usbctrl driver out-of-the-box.

## Hardware evidence (see `photos/`)

### Enumeration line printed by libxenon USB stack

```
USB: New device connected to bus 1 hub 1 port 1
USB bus 1 device 2: vendor 045E product 02AA class FF: Xbox 360 Controller
[USB] settle done.
```

### Button map captured in-band

| Button | Bits printed |
|---|---|
| A | `[PAD 0] ABXY 1000` |
| B | `[PAD 0] ABXY 0100` |
| X | `[PAD 0] ABXY 0010` |
| Y | `[PAD 0] ABXY 0001` |
| Back | `[PAD 0] SBLo 010` |
| Start | `[PAD 0] SBLo 100` |

Logo (central XBOX) not seen in the printed slice — either not pressed during the window, or logo signalling goes through a different path we did not probe. Not blocking; can revisit later.

### Final summary printed by the app

```
pad0 events: 35
pad1 events: 0
pad2 events: 0
pad3 events: 0
smc ir events: 0
DIAGNOSIS: usbctrl / SMC PATH WORKING.
Some input was seen.
```

## Root finding — clarifying ADR-006

`045E:02AA` is the **unified** VID/PID that a wireless Xbox 360 pad exposes when connected through *any* receiver, external OR internal. From the OHCI stack's viewpoint the internal RF module is just a USB port on the southbridge (Free60 wiki: "The two usb wires are connected to the southbridge"). libxenon already lists `045E:02AA` in `usbdevs.c` → driver `usbctrl_driver` binds → `get_controller_data()` returns real state.

**No `usbdevs.c` patch required.** No `XAM` / `XInput` fallback required for input. Bare-metal libxenon is viable for the full XCloud360 product on this dimension.

The label I originally attached to `02AA` in the inputdiag KNOWN_IDS table (`"Xbox 360 Wireless Controller (via receiver)"`) was misleading: it implied "external receiver only". Internal RF and external receiver are indistinguishable at the USB layer — both expose the same HID endpoints. Updating labels below.

## Also confirms

- Machine Check `P1.5-INPUT-001` on the original `inputdiag.elf32` was NOT an input-path bug. It was strictly in my manual walk of `usb_buses[]` / `ub_devices[]` + a long printf with `%s` args derived from library internals. The safe rewrite (public API only, ≤4 args per printf) runs stably and produces the answer.

## Status changes

- **ADR-006** → closed as `RESOLVED-POSITIVE` (usbctrl detects internal-RF-paired pads).
- **hwtest.elf32** input warning is now overly cautious — will soften on next rebuild.
- **P1.5-INPUT-001** stays documented in `crash-P1.5-INPUT-001/POSTMORTEM.md` as the lesson on user-code discipline (public API only), but the input capability itself is confirmed.

## Not tested yet (backlog)

- Central XBOX (Logo) button — likely goes through SMC power path or a separate HID report; probe when input polling matters at MVP.
- Analog sticks + triggers granularity + jitter — needed for §8 (~8ms polling, 125 Hz) later.
- Simultaneous multi-controller registration + slot assignment stability.
- Pad rumble via `usbctrl_set_rumble` (spec §8 does not mandate rumble but nice to have).

## Photos

`photos/`:
- `01-enumeration.png` (IMG_0196) — USB enumeration line + first 8s of pad events (A/B/X/Y)
- `02-buttons-abxy.png` (IMG_0197) — pad0 count climbing 11→19
- `03-back-start.png` (IMG_0198) — Back/Start being pressed (SBLo bits)
- `04-summary.png` (IMG_0199) — final SUMMARY block with DIAGNOSIS
