/*
 * XCloud360 v0.18 — game audio on the Xbox (make AUDIO=1).
 *
 * Packets: UDP :9004 from the PC bridge (capture-rtp), [seq u16 BE][rtp ts
 * u32 BE][Opus payload], received by the network thread straight into the
 * lock-free ring of aud_core. A future on-Xbox WebRTC stack calls
 * audio_rx_packet() instead — nothing else changes.
 *
 * Threads (see LOGIN-PROGRESS.md for the rest):
 *   - hw0 main: audio_init (boot: memory only, no hardware), then on the
 *     FIRST audio_session_begin it brings up xenon_sound + the Opus engine
 *     (breadcrumbs "[audio] 1/4..4/4" on screen); session begin/end, stats.
 *   - hw1 net: UDP :9004 pcb (created lazily by this thread, it owns lwIP),
 *     UDP callback (producer) and, from net_hook, audio_service() (consumer:
 *     jitter buffer + Opus decode + xenon_sound_submit). If there is no
 *     network thread the main thread calls audio_service() itself.
 * Any init failure = that session is silent; video/input are unaffected.
 */
#ifndef XCLOUD360_AUDIO_H
#define XCLOUD360_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_UDP_PORT 9004

/* AUDIO_SRC_RTC (stage 5): packets come from the on-Xbox WebRTC stack via
 * audio_rx_packet() (same [seq][ts][Opus] content as :9004); the :9004 pcb is
 * not opened for it. */
enum { AUDIO_SRC_NONE = 0, AUDIO_SRC_NET = 1, AUDIO_SRC_TEST = 2, AUDIO_SRC_RTC = 3 };

/* main, at boot: memory only (ring, test-stream index). Touches no hardware. */
int  audio_init(void);
/* main: start/stop a session. NET = play what arrives on :9004 (game);
 * TEST = play the embedded Opus tone/sweep stream in a loop (no PC needed).
 * The first begin brings up the sound chip and libopus (on this thread). */
void audio_session_begin(int src);
void audio_session_end(void);
/* Producer side for any other packet source (single producer). */
int  audio_rx_packet(uint16_t seq, uint32_t ts, const uint8_t *p, int len);
/* Consumer: call every ~1 ms from ONE thread. 1 = did an Opus decode. */
int  audio_service(void);
/* main: " a_pkts=.. a_lost=.. a_buf_ms=.. a_under=.. a_dec_ms=.." (STAT tail). */
int  audio_stat(char *dst, size_t n);
/* main: one human line for the menu after a session. */
void audio_summary(char *dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif
