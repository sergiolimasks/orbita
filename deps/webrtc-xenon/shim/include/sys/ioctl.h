/* XCloud360 stage 4 — <sys/ioctl.h> (Xenon shim). No ioctl() exists; only the
 * constants referenced by code that is compiled out without INET/INET6. */
#ifndef XCW_SHIM_SYS_IOCTL_H
#define XCW_SHIM_SYS_IOCTL_H
#define SIOCGIFMTU 0x8921
#endif
