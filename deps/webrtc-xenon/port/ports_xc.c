/*
 * XCloud360 stage 4 — replaces libpeer src/ports.c (+ mdns.c, which needs
 * multicast sockets). Host address, DNS, clock, sleep and randomness come
 * from xcw_port.h. Also provides:
 *   - peer_log() (libpeer built with LOG_REDIRECT=1)
 *   - MBEDTLS_TIMING_ALT functions (see ../mbedtls/timing_alt.h)
 *   - weak mbedtls_platform_gmtime_r (MBEDTLS_PLATFORM_GMTIME_R_ALT is part
 *     of the shared login config; source/login/https.c has the strong one).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/timing.h"

#include "ports.h"
#include "utils.h"
#include "xcw_port.h"

int ports_get_host_addr(Address* addr, const char* iface_prefix) {
  uint32_t ip = 0;
  (void)iface_prefix;
  if (addr->family != AF_INET && addr->family != 0) {
    return 0;
  }
  if (xcw_local_ipv4(&ip) != 0 || ip == 0) {
    return 0;
  }
  addr->family = AF_INET;
  addr->sin.sin_family = AF_INET;
  addr->sin.sin_addr.s_addr = htonl(ip);
  return 1;
}

int ports_resolve_addr(const char* host, Address* addr) {
  uint32_t ip = 0;
  if (xcw_resolve(host, &ip) != 0) {
    LOGE("resolve %s failed", host);
    return -1;
  }
  addr_set_family(addr, AF_INET);
  addr->sin.sin_family = AF_INET;
  addr->sin.sin_addr.s_addr = htonl(ip);
  return 0;
}

int ports_resolve_mdns_host(const char* host, Address* addr) {
  (void)host;
  (void)addr;
  return -1;
}

/* mdns.c replacement: ".local" candidates are not resolvable here (xCloud
 * never sends them). 0 = failure, matching mdns.c's convention. */
int mdns_resolve_addr(const char* hostname, Address* addr) {
  (void)hostname;
  (void)addr;
  return 0;
}

uint32_t ports_get_epoch_time() { return (uint32_t)xcw_now_ms(); }

void ports_sleep_ms(int ms) { xcw_idle(ms); }

void ports_random_bytes(void* buf, size_t n) { xcw_random(buf, n); }

/* ------------------------------------------------------------- logging */

void peer_log(char* level_tag, const char* file_name, int line_number, const char* fmt, ...) {
  char line[256];
  const char* base = strrchr(file_name, '/');
  base = base ? base + 1 : file_name;
  int n = snprintf(line, sizeof(line), "[peer %s %s:%d] ", level_tag, base, line_number);
  if (n < 0 || n >= (int)sizeof(line)) n = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
  va_end(ap);
  size_t l = strlen(line);
  while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
  xcw_log(line);
}

/* ------------------------------------------------- MBEDTLS_TIMING_ALT */

unsigned long mbedtls_timing_get_timer(struct mbedtls_timing_hr_time* val, int reset) {
  uint64_t now = xcw_now_ms();
  if (reset) {
    val->start_ms = now;
    return 0;
  }
  return (unsigned long)(now - val->start_ms);
}

void mbedtls_timing_set_delay(void* data, uint32_t int_ms, uint32_t fin_ms) {
  mbedtls_timing_delay_context* ctx = (mbedtls_timing_delay_context*)data;
  ctx->int_ms = int_ms;
  ctx->fin_ms = fin_ms;
  if (fin_ms != 0) {
    (void)mbedtls_timing_get_timer(&ctx->timer, 1);
  }
}

int mbedtls_timing_get_delay(void* data) {
  mbedtls_timing_delay_context* ctx = (mbedtls_timing_delay_context*)data;
  if (ctx->fin_ms == 0) {
    return -1;
  }
  unsigned long elapsed = mbedtls_timing_get_timer(&ctx->timer, 0);
  if (elapsed >= ctx->fin_ms) return 2;
  if (elapsed >= ctx->int_ms) return 1;
  return 0;
}

uint32_t mbedtls_timing_get_final_delay(const mbedtls_timing_delay_context* data) {
  return data->fin_ms;
}

/* ----------------------------------- MBEDTLS_PLATFORM_GMTIME_R_ALT (weak) */

__attribute__((weak)) struct tm* mbedtls_platform_gmtime_r(const mbedtls_time_t* tt, struct tm* tm_buf) {
  time_t t = (time_t)*tt;
  return gmtime_r(&t, tm_buf);
}
