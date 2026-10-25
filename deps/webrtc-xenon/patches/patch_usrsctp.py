#!/usr/bin/env python3
"""XCloud360 stage 4 — patch a COPY of usrsctp/usrsctplib for bare-metal Xenon.

usage: patch_usrsctp.py <copied usrsctplib dir>

Only needed for TARGET=xenon (XCW_BAREMETAL is force-defined by
deps/webrtc-xenon/shim/include/xcw_shim.h); host/ppc builds take the stock
Linux paths and the patched branches are dead code there.

  user_environment.c  read_random() from the port layer (xcw_random): newlib
                      has no getrandom()/arc4random on Xenon.
  sctp_constants.h    SCTP_GETTIME_TIMEVAL from xcw_now_us() (mftb): libxenon's
                      gettimeofday() asks the SMC for the RTC on every call.
"""
import sys, os

src = sys.argv[1]


def edit(fname, old, new):
    p = os.path.join(src, fname)
    with open(p, encoding='utf-8') as f:
        s = f.read()
    c = s.count(old)
    if c != 1:
        sys.exit('patch_usrsctp: %s: anchor found %d times:\n%s' % (fname, c, old[:200]))
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s.replace(old, new))


edit('user_environment.c', '#elif defined(__linux__)\n', '''#elif defined(XCW_BAREMETAL)
/* [XC360] bare-metal Xenon: CSPRNG from the WebRTC port layer (xcw_port.h). */
void xcw_random(void *buf, size_t n);

void
init_random(void)
{
	return;
}

void
read_random(void *buf, size_t size)
{
	xcw_random(buf, size);
}

void
finish_random(void)
{
	return;
}
#elif defined(__linux__)
''')
edit('netinet/sctp_constants.h', '''#else
#define SCTP_GETTIME_TIMEVAL(x)	gettimeofday(x, NULL)
#define SCTP_GETPTIME_TIMEVAL(x) gettimeofday(x, NULL)
#endif''', '''#elif defined(XCW_BAREMETAL)
/* [XC360] libxenon's gettimeofday() is an SMC round trip (ms resolution,
 * shared with every other SMC user); usrsctp calls this on per-packet paths
 * (RTT, timers). Use the port's monotonic mftb clock instead (xcw_port.h). */
#include <sys/time.h>
#include <stdint.h>
uint64_t xcw_now_us(void);
static inline void xcw_sctp_timeval(struct timeval *tv) {
	uint64_t u = xcw_now_us();
	tv->tv_sec = (time_t)(u / 1000000u);
	tv->tv_usec = (suseconds_t)(u % 1000000u);
}
#define SCTP_GETTIME_TIMEVAL(x)	xcw_sctp_timeval(x)
#define SCTP_GETPTIME_TIMEVAL(x) xcw_sctp_timeval(x)
#else
#define SCTP_GETTIME_TIMEVAL(x)	gettimeofday(x, NULL)
#define SCTP_GETPTIME_TIMEVAL(x) gettimeofday(x, NULL)
#endif''')
print('patch_usrsctp: ok')
