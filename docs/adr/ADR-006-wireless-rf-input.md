# ADR-006 — Wireless RF (internal module) input on bare-metal libxenon

**Status:** open — awaiting hardware evidence from `inputdiag`
**Date:** 2026-09-07

## Context

Test controller in this project = **original Xbox 360 wireless pad**, paired with the console's **internal RF module** (NOT a USB Wireless Gaming Receiver plugged into a front USB port). The user asked whether bare-metal libxenon supports reading input from a controller connected this way.

## What we found (code-confirmed)

### libxenon architecture (verified)

- `libxenon/drivers/input/input.c` — 25-line RAM relay. Provides `get_controller_data()` / `set_controller_data()` only. Reads no hardware itself.
- The **only** caller of `set_controller_data()` in the tree is `libxenon/drivers/usb/usbctrl.c` — a CFE-derived USB HID driver.
- `usbctrl.c` is bound at attach time via VID/PID matches in `libxenon/drivers/usb/usbdevs.c`:

| VID | PID | Product | libxenon table |
|---|---|---|---|
| `0x045e` | `0x028e` | Xbox 360 Wired Controller | ✅ `&usbctrl_driver` |
| `0x045e` | `0x02aa` | Wireless Controller (via receiver, variant A) | ✅ `&usbctrl_driver` |
| `0x045e` | `0x02a9` | Wireless Controller (via receiver, variant B) | ✅ `&usbctrl_driver` |
| `0x045e` | `0x02b0` | Kinect | ✅ `&dummy_driver` |
| `0x045e` | `0x028f` | Play & Charge Kit | ✅ `&dummy_driver` |
| `0x1bad` | `0xf900` | PDP Afterglow | ✅ `&usbctrl_driver` |
| `0x045e` | `0x0291` | Wireless Gaming Receiver (variant) | ❌ **NOT listed** |
| `0x045e` | `0x0719` | Xbox 360 Wireless Gaming Receiver (official) | ❌ **NOT listed** |

  usbctrl.c internally marks a device as `is_wireless` when `idProduct == 0x291`, but there is no attach entry for `0x291` or `0x0719` in usbdevs.c — the recv-driver code path exists but is not routable through the current driver table.

- Zero mentions in the libxenon tree of: XAM, XInput, internal RF, BCM4335, southbridge sync bus, SPI wireless. No dedicated internal-RF driver exists.

### Xbox 360 internal RF hardware (per Free60 wiki)

Source: <https://free60.org/Hardware/Console/RF_Module/> (fetched 2026-09-07).

> "The two usb wires are connected to the southbridge."

The RF module is **USB-attached to the southbridge** on internal pins — it is not a proprietary sync bus. From the OHCI stack's perspective the internal RF module SHOULD appear as a USB device on one of the root-hub ports (the internal one). Chips on the module: ATMEL 24C04 EEPROM + MS VS55RC / VS55RB (Microsoft custom). The RF module also handles the power-button signal and communicates with the SMC via a low-speed serial "front bus" (fb1/fb2).

Free60 does not document the VID/PID this module presents to OHCI — likely because it was never tested from bare-metal.

### Related upstream activity

- PR #47 (InvoxiPlayGames, 2025-09-30, **Draft**): adds Guitar/Wheel controller-type API + Xplorer Guitar `0x1430:0x4748` and RB Guitar `0x1bad:0x0002` to the driver table. **Does NOT add the wireless receiver PIDs.**
- No open issue or PR in Free60Project/libxenon that explicitly addresses the internal RF module.

## What we still don't know (needs hardware)

1. Does the internal RF module actually enumerate on one of the OHCI buses in bare-metal libxenon boot? (Free60 wiki says yes electrically; nobody has confirmed the software enumeration.)
2. If yes: what VID/PID does it present? Most likely `0x045e:0x0719` (same silicon as the external Wireless Gaming Receiver), but could differ.
3. Does the pad have to be *paired* with the internal RF module first, and does that pairing state survive across boots into XeLL / bare metal?

## Decision

Do NOT patch libxenon speculatively. Ship `inputdiag.elf32` (`app/inputdiag/`) — a diagnostic that:

- Enumerates every device on every OHCI bus (`usb_buses[]` + `bus->ub_devices[]`), prints `VID:PID class/subclass/protocol` + whether a driver is bound (`ud_drv != NULL`).
- Cross-references against a known-ID table (marks each device "in libxenon table" or "unknown/unbound").
- Polls `get_controller_data(0..3)` and `xenon_smc_get_ir()` for 20 s while the user presses every button, including the central XBOX button (which routes power events through the SMC).
- Reports counts + a suggested next step in-band.

Based on `inputdiag` output there are three possible outcomes and next steps:

| Outcome | Meaning | Next step |
|---|---|---|
| Unknown VID/PID appears on the bus (unbound) | Internal RF module IS enumerated but has no driver | Add its `{CLASS_ANY, VID, PID, &usbctrl_driver}` entry to `usbdevs.c`; rebuild libxenon; retest. Likely 1-line fix. |
| Known Microsoft device (e.g. `0x045e:0x0719`) shows unbound | Only usbdevs.c gap | Same 1-line patch. |
| No Microsoft device on any bus at all | Internal root-hub port not initialized bare-metal (OHCI enumeration doesn't reach the internal port), OR device signals present but wire-level negotiation fails | (a) try wired USB controller — validates full stack; (b) source a USB Wireless Gaming Receiver dongle for dev; (c) accept the limitation as evidence that native input requires XAM/XInput, i.e. push the final product to a XEX/kernel target for input parity — reinforces ADR-001 rationale. |

Do not preemptively rewrite the input stack. Wait for `inputdiag` evidence.

## Consequences

- `hwtest.elf32` may show `[INPUT] no button pressed in 3s` even when the user IS pressing buttons — this is expected until inputdiag identifies the missing driver bind and we patch libxenon or switch input source.
- If outcome is (c) above, the **product** (not just dev) needs to move to a XEX/kernel target for input. libxenon remains the R&D lab (CPU/VMX/network/render/decode) but the final input+audio+dashboard integration would need XAM.
- Regardless of outcome, wired USB controllers work today (VID `0x045e:0x028e` is in the table).

## Related

- Spec §8 (input requirements — 8 ms polling, all buttons + sticks + triggers)
- ADR-001 (toolchain — hybrid libxenon → OpenXeChain / possibly XEX final)
- Free60 wiki: <https://free60.org/Hardware/Console/RF_Module/>
- libxenon PR #47: <https://github.com/Free60Project/libxenon/pull/47>
