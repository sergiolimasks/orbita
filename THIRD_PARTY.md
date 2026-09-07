# Third-Party References

Sources consulted or intended to be derived-from. Every entry MUST list license, commit/version, and how it is used. Update on every integration.

## Referenced (not yet integrated)

| Project | Repo | License | Intended use | Status |
|---|---|---|---|---|
| Green-NX | rmrf404/green-nx | GPL-3.0 | xCloud native client arch (auth, WebRTC, H.264, Opus, gamepad, NACK/PLI) reference | study only |
| Stratix | nafields/stratix | TBD (verify) | xCloud protocol docs (`Docs/XCLOUD_PROTOCOL.md`), SDP, DataChannels, control/input/message | docs only |
| Better xCloud | redphx/better-xcloud | MIT | SDP manipulation, codec ordering, profile-level-id preferences | study only |
| XBMC-360 | brentdc-nz/XBMC-360 | GPL-2.0 (verify) | Xbox 360 XEX runtime, FFmpeg on Xenon, VMX128 kernels, threading, Xenos, audio | study — code derivation TBD |
| FFmpeg (PPC) | FFmpeg/FFmpeg | LGPL-2.1+ / GPL-2.1+ (build-dependent) | H.264 baseline decoder + PPC/AltiVec DSP (source for VMX128 port) | study — port TBD |
| libpeer | sepfy/libpeer | MIT | Embedded WebRTC (H.264, Opus, DataChannels, mbedTLS, libsrtp, usrsctp) | primary WebRTC candidate |
| libxenon | Free60Project/libxenon | GPL-2.0 (verify) | Xbox 360 runtime + xenon-gcc toolchain | primary bootstrap toolchain (ADR-001) |
| OpenXeChain | OpenXeChain/* | verify per-repo | Long-term LLVM/newlib/SynthXEX toolchain | future migration target |
| Xenia | xenia-project/xenia | BSD-3 | Xenos GPU reverse-engineering docs (register layouts, ringbuffer) | docs only |

## Derived / vendored code

None yet. Every future entry must record:
- source repo + commit hash
- license
- files touched
- destination path in this tree
- modifications summary
