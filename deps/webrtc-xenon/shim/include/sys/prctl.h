/* XCloud360 stage 4 — <sys/prctl.h> (Xenon shim). xenon-gcc predefines
 * __linux__ (its newlib target triple), so usrsctp takes Linux paths; thread
 * names are meaningless on bare metal. */
#ifndef XCW_SHIM_SYS_PRCTL_H
#define XCW_SHIM_SYS_PRCTL_H
#define PR_SET_NAME 15
static inline int prctl(int option, ...) { (void)option; return 0; }
#endif
