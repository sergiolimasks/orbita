/*
 * XCloud360 v0.18 — game audio on the Xbox (make AUDIO=1).
 *
 * Packets: [seq u16 BE][rtp ts u32 BE][Opus payload] from the on-Xbox
 * WebRTC stack (audio_rx_packet, network thread) into the lock-free ring of
 * aud_core. v0.20.0: the UDP :9004 path from the PC bridge is gone.
 *
 * Threads (see LOGIN-PROGRESS.md for the rest):
 *   - hw0 main: audio_init (boot: memory only, no hardware), then on the
 *     FIRST audio_session_begin it brings up xenon_sound + the Opus engine
 *     (breadcrumbs "[audio] 1/4..4/4" on screen); session begin/end, stats.
 *   - hw1 net: WebRTC callback (producer) and, from net_hook,
 *     audio_service() (consumer:
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

/* AUDIO_SRC_RTC (stage 5): packets from the on-Xbox WebRTC stack via
 * audio_rx_packet(). v0.20.0: AUDIO_SRC_NET (UDP :9004 from the PC) is gone;
 * the value 1 stays unused so the others keep their numbers. */
enum { AUDIO_SRC_NONE = 0, AUDIO_SRC_TEST = 2, AUDIO_SRC_RTC = 3 };

/* main, at boot: memory only (ring, test-stream index). Touches no hardware. */
int  audio_init(void);
/* main: start/stop a session. RTC = the game's WebRTC audio;
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
