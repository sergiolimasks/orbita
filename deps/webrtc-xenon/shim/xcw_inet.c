/* XCloud360 stage 4 — the few libc networking helpers newlib on Xenon lacks
 * (IPv4 text conversions + interface name stubs). Only built for TARGET=xenon. */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

static int parse_v4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || n > 255) return 0;
        }
        v = (v << 8) | n;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s != 0) return 0;
    *out = v;
    return 1;
}

int xcw_inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) return -1;   /* no IPv6 on Xenon (lwIP 1.4.1, IPv4 only) */
    uint32_t v;
    if (!parse_v4(src, &v)) return 0;
    ((struct in_addr *)dst)->s_addr = htonl(v);
    return 1;
}

const char *xcw_inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af == AF_INET) {
        const uint8_t *b = (const uint8_t *)src;   /* network order bytes */
        int n = snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return (n > 0 && (socklen_t)n < size) ? dst : NULL;
    }
    if (af == AF_INET6) {
        const uint8_t *b = (const uint8_t *)src;
        int n = 0;
        for (int i = 0; i < 16 && n >= 0 && (socklen_t)n < size; i += 2)
            n += snprintf(dst + n, size - (socklen_t)n, i ? ":%x" : "%x", (unsigned)(b[i] << 8 | b[i + 1]));
        return (n > 0 && (socklen_t)n < size) ? dst : NULL;
    }
    return NULL;
}

in_addr_t xcw_inet_addr(const char *cp) {
    uint32_t v;
    return parse_v4(cp, &v) ? htonl(v) : INADDR_NONE;
}

int xcw_inet_aton(const char *cp, struct in_addr *inp) {
    uint32_t v;
    if (!parse_v4(cp, &v)) return 0;
    inp->s_addr = htonl(v);
    return 1;
}

char *xcw_inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    xcw_inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return buf;
}

unsigned int if_nametoindex(const char *ifname) {
    (void)ifname;
    return 1;
}

char *if_indextoname(unsigned int ifindex, char *ifname) {
    if (ifindex != 1) return NULL;
    strcpy(ifname, "eth0");
    return ifname;
}

int getifaddrs(struct ifaddrs **ifap) {
    *ifap = NULL;
    return -1;
}

void freeifaddrs(struct ifaddrs *ifa) { (void)ifa; }
