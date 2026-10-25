/*
 * XCloud360 stage 3 — o pouco que a sessão precisa além do xc_port.h:
 * um lock pra caixa compartilhada entre a thread da sessão (hw 3), a de
 * rede (hw 1, WebRTC) e o main (menu).
 *   Xbox: xs_port_xenon.c (lock()/unlock() do libxenon + lwsync).
 *   host: hosttest/session/mock_port.c (pthread mutex).
 */
#ifndef XC360_XS_PORT_H
#define XC360_XS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

void xs_lock(void);
void xs_unlock(void);

#ifdef __cplusplus
}
#endif
#endif
