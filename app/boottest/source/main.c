/*
 * XCloud360 — boot smoke reproducer T6: netif guard + decoder init isolado.
 */
#include <stdio.h>
#include <string.h>
#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <usb/usbmain.h>
#include <network/network.h>
#include <lwip/ip_addr.h>

#include <libavcodec/avcodec.h>

extern struct netif netif;

int main(void) {
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    printf(" >> T3 A: xenos+console OK, libav LINKED (not called)\n");
    xenon_make_it_faster(XENON_SPEED_FULL);
    printf(" >> T3 B: make_it_faster OK\n");
    usb_init();
    printf(" >> T3 C: usb_init OK\n");
    usb_do_poll();
    printf(" >> T6 D: usb_do_poll OK\n");

    printf(" >> T6 E: netif.ip_addr = 0x%08x (0 = need init)\n",
        (unsigned)netif.ip_addr.addr);
    if (netif.ip_addr.addr == 0) {
        printf(" >> T6 F: calling network_init...\n");
        int r = network_init();
        printf(" >> T6 G: network_init returned %d\n", r);
    } else {
        printf(" >> T6 F: network already up (Aurora), skip init\n");
    }

    printf(" >> T6 H: about to avcodec_register_all...\n");
    avcodec_register_all();
    printf(" >> T6 I: avcodec_register_all OK\n");

    printf(" >> T6 J: about to avcodec_find_decoder(H264)...\n");
    AVCodec *codec = avcodec_find_decoder(CODEC_ID_H264);
    printf(" >> T6 K: find_decoder = %p\n", (void*)codec);
    if (codec) {
        printf(" >> T6 L: alloc_context3...\n");
        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        printf(" >> T6 M: ctx=%p, about to open2...\n", (void*)ctx);
        int op = avcodec_open2(ctx, codec, NULL);
        printf(" >> T6 N: open2 returned %d\n", op);
    }
    printf(" >> T6 Z: full decoder init OK, entering idle loop\n");

    for (;;) {
        usb_do_poll();
        udelay(50000);
    }
    return 0;
}
