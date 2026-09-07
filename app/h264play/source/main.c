/*
 * XCloud360 — P6/P7 end-to-end pipeline: H.264 decode + YUV→RGB (CPU) +
 * present on framebuffer via console_pset.
 *
 * This is the FIRST end-to-end proof: embedded H.264 CBP stream → libavcodec
 * decode → YUV420p planes → BT.601 CPU convert → 32bpp framebuffer.
 *
 * Perf target: 60 fps sustained (16.67 ms/frame budget).
 * If this fails, next step is (a) VMX128 acceleration of decode+convert
 * (P7.1), or (b) Xenos pixel-shader path (P7.2, requires proprietary
 * shadercompiler.exe from XNA — Sergio dependency).
 *
 * Output reports per second:
 *   [play] t+SSs dec=X.XX ms cvt=X.XX ms present=X.XX ms tot=Y.YY ms fps=ZZ.Z
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <input/input.h>
#include <usb/usbmain.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.1.0-h264play"
#endif

extern const uint8_t embedded_stream[];
extern const size_t embedded_stream_len;

#define FB_W 1280
#define FB_H 720

static uint64_t now_us(void) {
    return mftb() / (PPC_TIMEBASE_FREQ / 1000000ULL);
}

/* BT.601 SD YUV to RGB (video-range 16-235 Y, 16-240 UV).
 *   R = 1.164*(Y-16)               + 1.596*(V-128)
 *   G = 1.164*(Y-16) - 0.391*(U-128) - 0.813*(V-128)
 *   B = 1.164*(Y-16) + 2.018*(U-128)
 * We use integer fixed-point (Q10). Chroma is 4:2:0 (subsampled 2x in
 * both dims), so each U/V byte covers a 2x2 luma block. */
static inline uint8_t clip8(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Convert one 2-row strip (rows y..y+1) to RGB then present via console_pset.
 * console_pset takes cell x,y in pixels; it handles the Xenos tiled fb
 * layout internally (32x32 tiles), so per-pixel writes are safe (slow). */
static void convert_and_present_2rows(
    const uint8_t* Y0, const uint8_t* Y1,
    const uint8_t* U,  const uint8_t* V,
    int Y_stride, int UV_stride,
    int W, int y_row)
{
    for (int x = 0; x < W; x += 2) {
        int u = U[x/2] - 128;
        int v = V[x/2] - 128;
        int rc = ( 1596 * v + 512) >> 10;
        int gc = (-391 * u - 813 * v + 512) >> 10;
        int bc = ( 2018 * u + 512) >> 10;

        int y00 = 1164 * (Y0[x  ] - 16) >> 10;
        int y01 = 1164 * (Y0[x+1] - 16) >> 10;
        int y10 = 1164 * (Y1[x  ] - 16) >> 10;
        int y11 = 1164 * (Y1[x+1] - 16) >> 10;

        console_pset(x  , y_row  , clip8(y00 + rc), clip8(y00 + gc), clip8(y00 + bc));
        console_pset(x+1, y_row  , clip8(y01 + rc), clip8(y01 + gc), clip8(y01 + bc));
        console_pset(x  , y_row+1, clip8(y10 + rc), clip8(y10 + gc), clip8(y10 + bc));
        console_pset(x+1, y_row+1, clip8(y11 + rc), clip8(y11 + gc), clip8(y11 + bc));
    }
}

int main(void) {
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);
    usb_init();
    usb_do_poll();

    printf("\n\n==== XCloud360 h264play v%s ====\n\n", XCLOUD360_BUILD_VERSION);
    printf("[stream] %zu bytes embedded\n", embedded_stream_len);

    avcodec_register_all();
    AVCodec* codec = avcodec_find_decoder(CODEC_ID_H264);
    if (!codec) { printf("[err] no H264 decoder\n"); goto stop; }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        printf("[err] avcodec_open2 failed\n"); goto stop;
    }
    AVCodecParserContext* parser = av_parser_init(CODEC_ID_H264);
    AVFrame* frame = avcodec_alloc_frame();
    AVPacket pkt;
    av_init_packet(&pkt);

    printf("[play] pipeline: decode -> BT.601 CPU convert -> console_pset\n");
    printf("[play] target: 60fps (16.67 ms/frame). Reporting per second.\n\n");

    uint64_t t_wall_start = now_us();
    uint64_t t_next_report = t_wall_start + 1000000; /* +1s */
    uint64_t acc_dec = 0, acc_cvt = 0, acc_pres = 0;
    int frames_since_report = 0;
    int total_frames = 0;

    const uint8_t* data = embedded_stream;
    size_t         left = embedded_stream_len;

    while (left > 0) {
        uint8_t* out_data = NULL;
        int out_size = 0;
        int used = av_parser_parse2(parser, ctx,
            &out_data, &out_size, (uint8_t*)data, left,
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) break;
        data += used; left -= used;
        if (out_size <= 0) continue;

        pkt.data = out_data; pkt.size = out_size;

        uint64_t t_a = now_us();
        int got = 0;
        int r = avcodec_decode_video2(ctx, frame, &got, &pkt);
        uint64_t t_b = now_us();
        if (r < 0) break;
        if (!got) continue;

        int W = frame->width  < FB_W ? frame->width  : FB_W;
        int H = frame->height < FB_H ? frame->height : FB_H;
        int Yst  = frame->linesize[0];
        int UVst = frame->linesize[1];
        const uint8_t* Yp = frame->data[0];
        const uint8_t* Up = frame->data[1];
        const uint8_t* Vp = frame->data[2];

        uint64_t t_c = now_us();
        for (int y = 0; y < H; y += 2) {
            convert_and_present_2rows(
                Yp + y*Yst, Yp + (y+1)*Yst,
                Up + (y/2)*UVst, Vp + (y/2)*UVst,
                Yst, UVst, W, y);
        }
        uint64_t t_d = now_us();

        acc_dec  += (t_b - t_a);
        acc_cvt  += (t_d - t_c);   /* convert+present counted together */
        acc_pres += 0;
        frames_since_report++; total_frames++;

        if (t_d >= t_next_report) {
            double sec = (t_d - (t_next_report - 1000000)) / 1e6;
            double dec_ms = (double)acc_dec / 1000.0 / frames_since_report;
            double cvt_ms = (double)acc_cvt / 1000.0 / frames_since_report;
            double tot_ms = dec_ms + cvt_ms;
            double fps    = frames_since_report / sec;
            printf("[play] t+%2ds  dec=%.2f cvt=%.2f tot=%.2f ms/frame  fps=%.1f\n",
                (int)((t_d - t_wall_start)/1000000),
                dec_ms, cvt_ms, tot_ms, fps);
            acc_dec = acc_cvt = 0; frames_since_report = 0;
            t_next_report = t_d + 1000000;
        }
    }

    uint64_t t_wall_end = now_us();
    double wall_s = (t_wall_end - t_wall_start) / 1e6;
    double fps = total_frames / wall_s;
    printf("\n==== RESULT ====\n");
    printf("frames         : %d\n", total_frames);
    printf("wall time      : %.2f s\n", wall_s);
    printf("effective fps  : %.2f\n", fps);
    printf("target         : 60.00\n");
    if      (fps >= 55.0) printf("verdict        : PASS (>= 55fps)\n");
    else if (fps >= 30.0) printf("verdict        : PARTIAL (>= 30fps, needs SIMD)\n");
    else                  printf("verdict        : FAIL (< 30fps, SIMD mandatory)\n");

stop:
    printf("\n[end] press A to exit\n");
    for (;;) {
        struct controller_data_s c;
        usb_do_poll();
        get_controller_data(&c, 0);
        if (c.a) break;
        udelay(20000);
    }
    return 0;
}
