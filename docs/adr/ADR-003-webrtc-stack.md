# ADR-003 — WebRTC stack: libpeer (primary candidate)

**Status:** tentative (spec §5) — final acceptance after P11 port audit.

## Decision

Do NOT port Chromium/libwebrtc. Adopt `sepfy/libpeer` (MIT) as primary WebRTC stack. Deps: mbedTLS, libsrtp, usrsctp. Green-NX uses a similar embedded stack — direct architectural precedent.

## Prerequisites before commitment

Port audit must map libpeer dependencies against Xbox 360 runtime capabilities:

- pthread (libxenon provides — verify API surface)
- atomics (GCC 4.6 __sync_ builtins; C11 atomics unavailable)
- poll/select (libxenon lwIP — verify)
- monotonic clocks (Xenon TB register wrap behavior)
- filesystem (limited on Xbox — TBD if any TLS truststore needed on disk)
- random / entropy (libxenon RNG source — verify)
- endian assumptions (Xenon is BE — libpeer must be endian-clean)
- errno (multithread-safe?)
- DNS (lwIP resolver)
- BSD sockets (lwIP)

Any dependency that cannot be satisfied cleanly requires a compatibility shim in `platform/xbox360/`.

## Rejected alternatives

- Chromium/libwebrtc: dependencies (POSIX, syscalls, threading) incompatible with Xbox 360 runtime; binary size prohibitive.
- Rolling own WebRTC: reinventing ICE + DTLS-SRTP + SCTP + JSEP is months of work and Green-NX proved libpeer-class stacks are viable.

## Related

- Spec §5, §7, §11
