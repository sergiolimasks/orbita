/*
 * orbita v0.21.25 — decoder/CPU experiments from XC360/DIAG.TXT, the
 * decoder profile ([dec] prof, every 2 s) and the CPU budget ([cpu], every
 * 5 s). Parsing is pure C (hosttest/dec); the rest runs on the Xbox.
 * See DECODE-PERF.md.
 *
 * DIAG.TXT keys (one "key=value" per line; unknown keys are ignored):
 *   deblock=none|nonref|all   frames whose loop filter is SKIPPED
 *                             (none = normal; all = no deblock at all)
 *   decthreads=3|4            decoder frame threads in game (4 = hw3 joins
 *                             once the session thread left it)
 *   decprof=0|1               [dec] prof line (mftb counters in libavcodec)
 *   profile=default|baseline|main|high   H.264 profile asked in the offer
 *   copy=vmx|memcpy           YUV plane upload to the GPU textures
 *   spinprio=low|med          SMT priority of threads spinning idle in the
 *                             decoder waits (low = sibling gets the core)
 *   netprio=low|med           same for hw1 between network polls
 *   diag=full|lite            lite: fewer in-game logs/stats (see doc)
 */
#ifndef XC_DECDIAG_H
#define XC_DECDIAG_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XDD_DEBLOCK_NONE = 0, XDD_DEBLOCK_NONREF, XDD_DEBLOCK_ALL };
enum { XDD_PROFILE_DEFAULT = 0, XDD_PROFILE_BASELINE, XDD_PROFILE_MAIN, XDD_PROFILE_HIGH };

typedef struct {
    int deblock;        /* XDD_DEBLOCK_* (default: build's SKIP_DEBLOCK) */
    int decthreads;     /* 0 = build default */
    int prof;           /* 1 = [dec] prof line */
    int profile;        /* XDD_PROFILE_* */
    int copy_vmx;       /* 1 = VMX + dcbz plane upload */
    int spin_low;       /* 1 = low SMT priority while idle-spinning (decoder) */
    int net_low;        /* 1 = low SMT priority for hw1 between polls */
    int lite;           /* 1 = diag=lite */
    int netlog_set;     /* the file had netlog= (lite does not override it) */
    int keys;           /* how many known keys were read */
} xdd_cfg_t;

extern xdd_cfg_t xdd;

void xdd_defaults(xdd_cfg_t *c);
/* parses DIAG.TXT text into c (starting from its current values) */
void xdd_parse(xdd_cfg_t *c, const char *txt);
void xdd_describe(const xdd_cfg_t *c, char *out, size_t n);
const char *xdd_profile_name(int p);
/* profile-level-id for the offer ("42e01f"...), NULL = keep the default */
const char *xdd_profile_plid(int p);
/* H.264 SPS profile_idc -> name */
const char *xdd_h264_profile_name(int profile_idc);

#ifdef XENON
struct AVCodecContext;
/* boot: reads the parsed config into the switches of the other modules */
void xdd_boot_apply(void);
/* decoder context before avcodec_open2 (deblock, flags) */
void xdd_apply_ctx(struct AVCodecContext *ctx);
/* v0.21.26: avcodec_open2 com as threads de frame presas às hw 4,2,5 (+3
 * se hw3 = 1, aluguel na mão); tenta menos threads se faltar; loga o mapa */
struct AVCodec;
int  xdd_open_pinned(struct AVCodecContext *ctx, struct AVCodec *codec, int hw3);
/* decoder threads cap in game */
int  xdd_max_threads(int build_default);
/* every STAT period (2 s) in game: [dec] prof; every 5 s: [cpu] */
void xdd_tick(uint64_t now_us, int dec_threads, unsigned gpu_wait_us_total);
/* accumulators the app feeds (mftb ticks) */
extern volatile uint64_t xdd_tb_main_idle, xdd_tb_audio, xdd_tb_pad, xdd_tb_usbwifi;
extern volatile uint32_t xdd_n_audio, xdd_n_pad;
/* "lite" in game: drop this library log line? (keeps errors) */
int  xdd_drop_lib_line(const char *line, int in_game);
#endif

#ifdef __cplusplus
}
#endif
#endif
