# libpeer-xenon.patch — deferred, must be authored by hand

The green-nx libpeer-switch.patch is 51KB and does 3 fundamentally different
things at once. For Xenon we replicate the same 3 pieces, but the CONTENT of
each differs. The full patch is deferred; the plan is:

## Piece 1 — CMakeLists.txt rewrite (~50 lines, easy)
Delete the whole PC-side `ExternalProject_Add` graph (already rebuilds cjson,
mbedtls, srtp2, usrsctp — we do that separately in build-libpeer-xenon.sh).
Keep only:
- `add_library(peer STATIC ${sources})`
- `target_include_directories(peer PRIVATE src include ${MBEDTLS_INCLUDE_DIR})`
- `target_compile_definitions(peer PRIVATE CONFIG_USE_LWIP=1 CONFIG_USE_USRSCTP=1)`
- `if(DISABLE_PEER_SIGNALING) ... else() include coreHTTP/coreMQTT ... endif()`
- `install(TARGETS peer ARCHIVE DESTINATION lib/ PUBLIC_HEADER DESTINATION include/)`

Copy the green-nx patch verbatim starting at line 1 of libpeer-switch.patch —
it's target-independent CMake refactor.

## Piece 2 — src/socket.c refactor (~400 lines, MEDIUM)
`libxenon` lwip 1.4.1 has `LWIP_SOCKET=0`, no `socket()`/`bind()`/`recvfrom()`.
Must rewrite `UdpSocket` and `TcpSocket` to use lwip RAW API (already familiar
from `app/net/tcp_stream_client.c`):

Replacement primitives:
- `socket(AF_INET, SOCK_DGRAM, 0)` → `udp_new()` returning `struct udp_pcb*`
- `bind(fd, sa, len)`              → `udp_bind(pcb, IP_ADDR_ANY, port)`
- `recvfrom(...)`                  → `udp_recv(pcb, cb, arg)` — cb writes into
                                     a lock-free ring (single-thread cooperative)
- `sendto(...)`                    → build `pbuf`, `udp_sendto(pcb, pb, ipaddr, port)`
- `setsockopt(SO_RCVBUF)`          → no-op (lwip has `pcb_recv_bytes` config)
- `close(fd)`                      → `udp_remove(pcb)`

Same pattern for TCP: `tcp_new()`, `tcp_bind()`, `tcp_recv()`, `tcp_write()`,
`tcp_output()`.

CRITICAL: libpeer expects a SYNCHRONOUS `recvfrom()` that returns bytes. RAW
API delivers asynchronously via callback. Two options:
(a) Adapter: `udp_socket_recvfrom()` drains a ring the recv callback fills.
    Caller must invoke `network_poll()` between iterations. Cleanest.
(b) Rewrite `agent_socket_recv()` upstack to be event-driven. Bigger change.

**Recommended**: option (a). Matches tcp_stream_client.c model already in
XCloud360 code. Add `void udp_socket_pump(void) { network_poll(); }` and call
from libpeer's DTLS handshake loop.

## Piece 3 — src/ports.c refactor (~30 lines, easy)
`ports_get_host_addr()` walks `getifaddrs()`. Xenon: no getifaddrs. Replace
with `netif_list` walk (LWIP already exports it, see how green-nx does the
same block conditionally on `CONFIG_USE_LWIP`).

`ports_resolve_addr()` (DNS): use `netconn_gethostbyname()` — wait, LWIP_NETCONN=0
on xenon too. Use lwip RAW `dns_gethostbyname()` with a callback + short spin.
DNS unlikely to be needed if you're doing static signaling server IP.

`ports_sleep_ms()`: use `mftb()` busy-wait or a udelay from libxenon.
`ports_get_epoch_time()`: `mftb() / (PPC_TIMEBASE_FREQ)`; note timebase does
NOT survive reboot, so this is a "seconds since boot" not epoch. mbedtls
DTLS-SRTP does NOT care about wall-clock (MBEDTLS_HAVE_TIME_DATE off).

## Author strategy

1. Copy green-nx `libpeer-switch.patch` piece 1 verbatim (CMake rewrite).
2. Copy pieces from the patch marked with `[XCLOUD360-PATCH]` already in
   `~/libpeer/src/dtls_srtp.c` (bio callback, role switch, ECDSA) — already
   applied, don't touch.
3. Write socket.c refactor as a NEW file `src/socket_xenon.c` and use
   `#if defined(__XENON__)` at the top of `src/socket.c` to conditionally
   compile out the BSD path (green-nx pattern for socket.c body — use the
   RCVBUF patch already in ~/libpeer/src/socket.c as-is where possible).
4. Write ports.c LWIP branch as `#if CONFIG_USE_LWIP && defined(__XENON__)`.

Estimated effort: 4-6h focused. Not a blocker for build system — start with
CMake rewrite, get link errors, fix bottom-up.
