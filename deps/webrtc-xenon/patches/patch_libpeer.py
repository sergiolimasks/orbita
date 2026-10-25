#!/usr/bin/env python3
"""XCloud360 stage 4 — patch a COPY of ~/libpeer/src (already carrying the
green-nx + XCloud360 RTCP-helper changes) for the bare-metal WebRTC build.

Exact-anchor string edits: every anchor must match exactly once, otherwise the
script aborts (so an upstream change can never be silently half-applied).

usage: patch_libpeer.py <copied libpeer src dir>

What changes and why (all [XC360] comments in the output):
  agent.c        no select(): the UDP port is non-blocking (0 = nothing yet);
                 STUN gathering waits with ports_sleep_ms(1) between polls;
                 STUN PRIORITY attribute = 32-bit network-order local priority
                 (upstream wrote the first 4 bytes of a uint64 in host order:
                 0 on big-endian Xenon); a valid binding RESPONSE refreshes
                 the keepalive timer (consent freshness, opt-in).
  dtls_srtp.*    handshake is a STEP function (one mbedtls_ssl_handshake per
                 peer_connection_loop call, returns 1 while in progress) so it
                 never blocks the caller for up to 15 s; timer moved into the
                 struct; SRTP protect/unprotect return the libsrtp status;
                 certificate validity is ignored (the SDP fingerprint is the
                 authenticator, the Xbox clock may be unset).
  peer_connection.c  drops packets that fail SRTP auth/replay (upstream fed
                 still-encrypted bytes to the media callbacks) and counts them;
                 remembers the last handshake step result; a remote candidate
                 added while CHECKING gets a candidate pair (upstream: ignored);
                 set_remote_description copies each SDP line bounded (upstream
                 overflowed a 256-byte stack buffer on a long line).
  sctp.c         usrsctp_init_nothreads() + sctp_usrsctp_handle_timers(): no
                 timer thread on bare metal, the caller drives SCTP timers.
  utils.c        ICE ufrag/pwd from ports_random_bytes() (upstream: rand()
                 seeded with time(NULL) -> identical credentials every boot on
                 a console without a set clock).
"""
import sys, os

src = sys.argv[1]
n_edits = 0


def edit(fname, old, new, count=1):
    global n_edits
    p = os.path.join(src, fname)
    with open(p, encoding='utf-8') as f:
        s = f.read()
    c = s.count(old)
    if c != count:
        sys.exit('patch_libpeer: %s: anchor found %d times (want %d):\n%s' % (fname, c, count, old[:200]))
    s = s.replace(old, new)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)
    n_edits += 1


def replace_between(fname, start, end, new):
    """Replace text from `start` (inclusive) up to `end` (exclusive)."""
    global n_edits
    p = os.path.join(src, fname)
    with open(p, encoding='utf-8') as f:
        s = f.read()
    if s.count(start) != 1 or s.count(end) != 1:
        sys.exit('patch_libpeer: %s: region anchors not unique: %r / %r' % (fname, start[:60], end[:60]))
    a = s.index(start)
    b = s.index(end)
    if b <= a:
        sys.exit('patch_libpeer: %s: region end before start' % fname)
    s = s[:a] + new + s[b:]
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)
    n_edits += 1


# ------------------------------------------------------------------ agent.c
edit('agent.c', '#include <sys/select.h>\n', '/* [XC360] no select(): non-blocking xcw_port UDP */\n')

replace_between('agent.c', 'static int agent_socket_recv(Agent* agent, Address* addr, uint8_t* buf, int len) {',
                'static int agent_socket_recv_attempts(', '''static int agent_socket_recv(Agent* agent, Address* addr, uint8_t* buf, int len) {
  /* [XC360] Non-blocking poll of the IPv4 socket (no IPv6 on Xenon). Returns
   * >0 bytes, 0 = nothing waiting (upstream: select() timeout), <0 error.
   * The caller (xc_webrtc_poll) paces the handshake states to ~1 call/ms so
   * the AGENT_CONNCHECK_* loop counters keep their upstream meaning. */
  if (agent->udp_sockets[0].fd < 0)
    return -1;
  return udp_socket_recvfrom(&agent->udp_sockets[0], addr, buf, len);
}

''')

edit('agent.c', '''    if ((ret = agent_socket_recv(agent, addr, buf, len)) != 0) {
      break;
    }
''', '''    if ((ret = agent_socket_recv(agent, addr, buf, len)) != 0) {
      break;
    }
    ports_sleep_ms(1); /* [XC360] was the select() timeout */
''')

edit('agent.c', '''  stun_msg_write_attr(msg, STUN_ATTR_TYPE_PRIORITY, 4, (char*)&agent->nominated_pair->priority);''',
     '''  /* [XC360] 32-bit, network order (upstream: first 4 bytes of a host-order
   * uint64 -> 0 on big-endian). */
  uint32_t prio_be = htonl(agent->nominated_pair->local ? agent->nominated_pair->local->priority
                                                         : (uint32_t)agent->nominated_pair->priority);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_PRIORITY, 4, (char*)&prio_be);''')

edit('agent.c', '''      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->remote_upwd) == 0) {
        agent->nominated_pair->state = ICE_CANDIDATE_STATE_SUCCEEDED;
      }''', '''      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->remote_upwd) == 0) {
        agent->nominated_pair->state = ICE_CANDIDATE_STATE_SUCCEEDED;
        /* [XC360] an answered consent check (RFC 7675) also proves the peer
         * is alive. Only matters when the caller sends consent requests
         * (xcw cfg.consent_interval_ms); upstream only counted the peer's own
         * binding requests towards CONFIG_KEEPALIVE_TIMEOUT. */
        agent->binding_request_time = ports_get_epoch_time();
      }''')

# -------------------------------------------------------------- dtls_srtp.h
edit('dtls_srtp.h', '''  char actual_remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];

  void* user_data;
''', '''  char actual_remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];

  /* [XC360] step-mode handshake state + its retransmission timer */
  int hs_started;
  mbedtls_timing_delay_context hs_timer;

  void* user_data;
''')
edit('dtls_srtp.h', '''void dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

void dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

void dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

void dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);''',
     '''/* [XC360] return the libsrtp status (0 = ok) */
int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

/* [XC360] dtls_srtp_handshake(): 0 = done, 1 = in progress (call again), <0 = failed */''')

# -------------------------------------------------------------- dtls_srtp.c
edit('dtls_srtp.c', '''  *flags &= ~(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH | MBEDTLS_X509_BADCERT_BAD_KEY);''',
     '''  /* [XC360] + EXPIRED/FUTURE: WebRTC authenticates by SDP fingerprint, and
   * the Xbox wall clock may be unset when the stream starts. */
  *flags &= ~(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH | MBEDTLS_X509_BADCERT_BAD_KEY |
              MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);''')

edit('dtls_srtp.c', '''  dtls_srtp->role = role;
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
''', '''  dtls_srtp->role = role;
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
  dtls_srtp->hs_started = 0; /* [XC360] */
''')

replace_between('dtls_srtp.c', 'static int dtls_srtp_do_handshake(DtlsSrtp* dtls_srtp) {', 'void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp) {', '''/* [XC360] Step-mode handshake. Upstream looped here (usleep(1000) between
 * mbedtls_ssl_handshake calls, up to 15 s) which would freeze the Xbox
 * network thread. Now every call does ONE mbedtls_ssl_handshake() step and
 * returns 1 while the handshake is in progress; the retransmission timer
 * (1/2/4 s, see dtls_srtp_init) still fires because the caller keeps
 * stepping. Retrying after a failure = calling again (state restarts). */
static void dtls_srtp_hs_begin(DtlsSrtp* dtls_srtp) {
  if (dtls_srtp->state == DTLS_SRTP_STATE_CONNECTED) {
    /* keys of a previous (failed late) attempt */
    srtp_dealloc(dtls_srtp->srtp_in);
    srtp_dealloc(dtls_srtp->srtp_out);
    dtls_srtp->state = DTLS_SRTP_STATE_INIT;
  }
  mbedtls_ssl_session_reset(&dtls_srtp->ssl);
  mbedtls_ssl_set_timer_cb(&dtls_srtp->ssl, &dtls_srtp->hs_timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
#if CONFIG_MBEDTLS_2_X
  mbedtls_ssl_conf_export_keys_ext_cb(&dtls_srtp->conf, dtls_srtp_key_derivation_cb, dtls_srtp);
#else
  mbedtls_ssl_set_export_keys_cb(&dtls_srtp->ssl, dtls_srtp_key_derivation_cb, dtls_srtp);
#endif
  mbedtls_ssl_set_bio(&dtls_srtp->ssl, dtls_srtp, dtls_srtp->udp_send, dtls_srtp->udp_recv, NULL);
  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    static const unsigned char client_id[] = "test";
    mbedtls_ssl_set_client_transport_id(&dtls_srtp->ssl, client_id, sizeof(client_id));
  }
  dtls_srtp->state = DTLS_SRTP_STATE_HANDSHAKE;
  dtls_srtp->hs_started = 1;
}

int dtls_srtp_handshake(DtlsSrtp* dtls_srtp, Address* addr) {
  int ret;
  dtls_srtp->remote_addr = addr;

  if (!dtls_srtp->hs_started)
    dtls_srtp_hs_begin(dtls_srtp);

  ret = mbedtls_ssl_handshake(&dtls_srtp->ssl);
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    return 1;
  if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
    LOGD("DTLS hello verification requested");
    dtls_srtp->hs_started = 0; /* server: restart with the cookie */
    return 1;
  }
  dtls_srtp->hs_started = 0;
  if (ret != 0) {
    LOGE("failed! mbedtls_ssl_handshake returned -0x%.4x", (unsigned int)-ret);
    return ret < 0 ? ret : -1;
  }

  const mbedtls_x509_crt* remote_crt;
  if ((remote_crt = mbedtls_ssl_get_peer_cert(&dtls_srtp->ssl)) != NULL) {
    dtls_srtp_x509_digest(remote_crt, dtls_srtp->actual_remote_fingerprint);

    if (strncmp(dtls_srtp->remote_fingerprint, dtls_srtp->actual_remote_fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH) != 0) {
      LOGE("Actual and Expected Fingerprint mismatch: %s %s",
           dtls_srtp->remote_fingerprint,
           dtls_srtp->actual_remote_fingerprint);
      return -2;
    }

  } else {
    LOGE("no remote fingerprint");
    return -3;
  }

  if (dtls_srtp->state != DTLS_SRTP_STATE_CONNECTED) {
    LOGE("handshake done but no SRTP keys exported");
    return -4;
  }

  LOGI("DTLS %s handshake done", dtls_srtp->role == DTLS_SRTP_ROLE_SERVER ? "server" : "client");
  return 0;
}

''')

edit('dtls_srtp.c', '''void dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  srtp_unprotect(dtls_srtp->srtp_in, packet, bytes);
}

void dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  srtp_unprotect_rtcp(dtls_srtp->srtp_in, packet, bytes);
}

void dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  srtp_protect(dtls_srtp->srtp_out, packet, bytes);
}

void dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  srtp_protect_rtcp(dtls_srtp->srtp_out, packet, bytes);
}''', '''int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return (int)srtp_unprotect(dtls_srtp->srtp_in, packet, bytes);
}

int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return (int)srtp_unprotect_rtcp(dtls_srtp->srtp_in, packet, bytes);
}

int dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return (int)srtp_protect(dtls_srtp->srtp_out, packet, bytes);
}

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return (int)srtp_protect_rtcp(dtls_srtp->srtp_out, packet, bytes);
}''')

# ---------------------------------------------------------- peer_connection.c
edit('peer_connection.c', '''  uint32_t rtcp_last_sr_lsr;
  uint32_t rtcp_last_sr_time;
};''', '''  uint32_t rtcp_last_sr_lsr;
  uint32_t rtcp_last_sr_time;

  /* [XC360] */
  uint32_t srtp_errors;
  int dtls_hs_ret;
};

uint32_t peer_connection_get_srtp_errors(PeerConnection* pc) { return pc->srtp_errors; }
int peer_connection_get_dtls_step(PeerConnection* pc) { return pc->dtls_hs_ret; }
uint32_t peer_connection_get_remote_video_ssrc(PeerConnection* pc) { return pc->remote_vssrc; }
uint32_t peer_connection_get_remote_audio_ssrc(PeerConnection* pc) { return pc->remote_assrc; }''')

edit('peer_connection.c', '''      if (dtls_srtp_handshake(&pc->dtls_srtp, NULL) == 0) {''',
     '''      /* [XC360] step-mode: 1 = in progress, <0 = attempt failed (the next
       * call restarts it), 0 = done */
      if ((pc->dtls_hs_ret = dtls_srtp_handshake(&pc->dtls_srtp, NULL)) == 0) {''')

edit('peer_connection.c', '''          dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
          peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);''',
     '''          if (dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret) == 0)
            peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);
          else
            pc->srtp_errors++; /* [XC360] */''')

edit('peer_connection.c', '''          dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
''', '''          if (dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret) != 0) {
            /* [XC360] auth/replay failure: never hand ciphertext to the
             * media callbacks. agent_ret stays > 0 (a packet was consumed). */
            pc->srtp_errors++;
            break;
          }
''')

edit('peer_connection.c', '''  LOGD("Add candidate: %s", candidate);
  agent->remote_candidates_count++;
  return 0;''', '''  LOGD("Add candidate: %s", candidate);
  agent->remote_candidates_count++;
  /* [XC360] trickled AFTER set_remote_description: upstream builds pairs only
   * there, so a late candidate was never checked. agent_update_candidate_pairs
   * only appends pairs for new endpoints. */
  if (pc->state == PEER_CONNECTION_CHECKING)
    agent_update_candidate_pairs(agent);
  return 0;''')

edit('peer_connection.c', '''    line = strstr(start, "\\r\\n");
    strncpy(buf, start, line - start);
    buf[line - start] = '\\0';''', '''    line = strstr(start, "\\r\\n");
    /* [XC360] bounded: an answer line >= 256 bytes overflowed buf[256] on
     * the stack (fatal on the Xbox network thread) */
    size_t line_len = (size_t)(line - start) < sizeof(buf) - 1 ? (size_t)(line - start) : sizeof(buf) - 1;
    memcpy(buf, start, line_len);
    buf[line_len] = '\\0';''')

# ------------------------------------------------------- peer_connection.h
edit('peer_connection.h', '''int peer_connection_add_ice_candidate(PeerConnection* pc, char* ice_candidate);''',
     '''int peer_connection_add_ice_candidate(PeerConnection* pc, char* ice_candidate);

/* [XC360] diagnostics */
uint32_t peer_connection_get_srtp_errors(PeerConnection* pc);
int peer_connection_get_dtls_step(PeerConnection* pc);
uint32_t peer_connection_get_remote_video_ssrc(PeerConnection* pc);
uint32_t peer_connection_get_remote_audio_ssrc(PeerConnection* pc);
/* [XC360] usrsctp has no timer thread on bare metal: call with the ms
 * elapsed since the previous call (sctp.c). */
void sctp_usrsctp_handle_timers(uint32_t elapsed_ms);''')

# ------------------------------------------------------------------- sctp.c
edit('sctp.c', '''  usrsctp_init(0, sctp_outgoing_data_cb, NULL);''',
     '''  /* [XC360] no "SCTP timer"/"iterator" threads on bare metal: the caller
   * drives timers through sctp_usrsctp_handle_timers(). */
  usrsctp_init_nothreads(0, sctp_outgoing_data_cb, NULL);''')
edit('sctp.c', '''    usleep(20 * 1000);''', '''    ports_sleep_ms(20); /* [XC360] */''')
edit('sctp.c', '''#include "dtls_srtp.h"
#include "sctp.h"''', '''#include "dtls_srtp.h"
#include "ports.h"
#include "sctp.h"''')
with open(os.path.join(src, 'sctp.c'), 'a', encoding='utf-8') as f:
    f.write('''
/* [XC360] */
void sctp_usrsctp_handle_timers(uint32_t elapsed_ms) {
#if CONFIG_USE_USRSCTP
  usrsctp_handle_timers(elapsed_ms);
#else
  (void)elapsed_ms;
#endif
}
''')
n_edits += 1

# ------------------------------------------------------------------ ports.h
edit('ports.h', '''void ports_sleep_ms(int ms);''', '''void ports_sleep_ms(int ms);

/* [XC360] CSPRNG bytes (xcw_random) */
void ports_random_bytes(void* buf, size_t n);''')

# ------------------------------------------------------------------ utils.c
edit('utils.c', '''  srand(time(NULL));

  for (i = 0; i < len; ++i) {
    s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
  }''', '''  /* [XC360] CSPRNG instead of rand()/srand(time(NULL)) */
  unsigned char rnd[256];
  int n = len < (int)sizeof(rnd) ? len : (int)sizeof(rnd);
  ports_random_bytes(rnd, (size_t)n);
  for (i = 0; i < len; ++i) {
    s[i] = alphanum[rnd[i % n] % (sizeof(alphanum) - 1)];
  }''')
edit('utils.c', '''#include "mbedtls/md.h"''', '''#include "mbedtls/md.h"
void ports_random_bytes(void* buf, size_t n); /* [XC360] ports_xc.c */''')

print('patch_libpeer: %d edits applied to %s' % (n_edits, src))
