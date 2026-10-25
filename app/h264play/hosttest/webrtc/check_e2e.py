#!/usr/bin/env python3
"""XCloud360 stage 4 — verdict for one offline E2E run (run_e2e.sh).

Compares what the pion peer SENT with what the Xbox-side receiver DELIVERED:
  video  every delivered AU must be byte-identical (as a NAL list) to a sent
         AU, in order; reports missing AUs (dropped by jitter/IDR gating);
  audio  every delivered Opus payload must match a sent one, in order;
  input  pion's validation of the "input" reports (length/type/sequence);
  dc     ping/pong round trips on "chat", control + startup messages seen;
  rtcp   PLI / NACK / REMB / RR as received by pion.
usage: check_e2e.py <run dir> <induced loss fraction>
Prints a summary and exits 1 on any hard failure.
"""
import hashlib, json, os, struct, sys

d = sys.argv[1]
loss = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
fails = []


def nals(b):
    out, i, start = [], 0, -1
    while i + 3 <= len(b):
        if b[i] == 0 and b[i + 1] == 0 and b[i + 2] == 1:
            if start >= 0:
                e = i
                while e > start and b[e - 1] == 0:
                    e -= 1
                out.append(b[start:e])
            i += 3
            start = i
            continue
        i += 1
    if 0 <= start < len(b):
        out.append(b[start:])
    return out


def au_hash(nl):
    h = hashlib.sha1()
    for n in nl:
        t = n[0] & 0x1f
        if t in (9, 12):
            continue
        h.update(struct.pack('>I', len(n)))
        h.update(n)
    return h.hexdigest()


recv = json.load(open(os.path.join(d, 'recv.json')))
pion = json.load(open(os.path.join(d, 'pion.json')))
sent = [l.split() for l in open(os.path.join(d, 'sent.auidx')) if l.strip()]
idx = [l.split() for l in open(os.path.join(d, 'recv.auidx')) if l.strip()]
data = open(os.path.join(d, 'recv.h264'), 'rb').read()

# ---- video
pos, j, matched, corrupt, prepended, first_k, last_k = 0, 0, 0, 0, 0, None, None
for size, ts, idr, pre in idx:
    size = int(size)
    nl = nals(data[pos:pos + size])
    pos += size
    hs = [au_hash(nl)]
    if pre == '1':
        prepended += 1
        k = 0
        while k < len(nl) and (nl[k][0] & 0x1f) in (7, 8):
            k += 1
        hs.append(au_hash(nl[k:]))
        hs.append(au_hash(nl[2:]))
    k = j
    while k < len(sent) and sent[k][0] not in hs:
        k += 1
    if k == len(sent):
        corrupt += 1
        continue
    matched += 1
    first_k = k if first_k is None else first_k
    last_k = k
    j = k + 1
span = (last_k - first_k + 1) if matched else 0
missing = span - matched
print('video : sent %d AUs, delivered %d, byte-identical %d, corrupt %d, missing in span %d (%.2f%%), '
      'params-prepended %d' % (len(sent), len(idx), matched, corrupt, missing, 100.0 * missing / span if span else 0,
                               prepended))
if not matched:
    fails.append('no video delivered')
if corrupt:
    fails.append('%d corrupt AUs' % corrupt)
if loss == 0 and missing:
    fails.append('%d AUs missing without loss' % missing)
once = os.environ.get('XCW_LOSS_ONCE') == '1'
if loss > 0:
    # green-nx/capture-rtp semantics: ONE NACK per gap; if the retransmission
    # is lost too the frame times out (100 ms), is dropped and the buffer waits
    # for the next IDR (keyframe request throttled 1/s). So with independent
    # loss p a frame is lost ~p^2 per packet; with XCW_LOSS_ONCE every loss
    # must be recovered.
    print('nack  : %d induced drops -> %d frames given up (%.1f%% of drops not recovered)' %
          (recv['udp_dropped'], recv['jit_dropped'], 100.0 * recv['jit_dropped'] / max(1, recv['udp_dropped'])))
    if once and recv['jit_dropped']:
        fails.append('loss-once: %d frames not recovered by NACK' % recv['jit_dropped'])
    if not once and recv['jit_dropped'] > 0.2 * recv['udp_dropped']:
        fails.append('NACK recovery below 80%%')

# ---- audio
sent_a = [l.strip() for l in open(os.path.join(d, 'sent.opus.idx')) if l.strip()]
ra = open(os.path.join(d, 'recv.opus'), 'rb').read()
p, am, ac, ja = 0, 0, 0, 0
while p + 8 <= len(ra):
    seq, ts, n = struct.unpack('>HIH', ra[p:p + 8])
    h = hashlib.sha1(ra[p + 8:p + 8 + n]).hexdigest()
    p += 8 + n
    k = ja
    while k < len(sent_a) and sent_a[k] != h:
        k += 1
    if k == len(sent_a):
        ac += 1
    else:
        am += 1
        ja = k + 1
print('audio : sent %d Opus, delivered %d, identical %d, corrupt %d' % (len(sent_a), recv['audio_packets'], am, ac))
if am == 0 or ac:
    fails.append('audio mismatch')

# ---- transport / timings
print('setup : offer %.1f ms; after answer: ICE %d ms, DTLS %d ms, SCTP %d ms, HandshakeAck %d ms, first AU %d ms' %
      (recv['t_offer_ms'], recv['t_ice_ms'], recv['t_dtls_ms'], recv['t_sctp_ms'], recv['t_handshake_ms'],
       recv['t_first_au_ms']))
print('rtp   : %d video RTP pkts, udp rx %d, induced drops %d, NACKs sent %d (pion saw %d NACK pkts / %d seqs), '
      'jitter drops %d, resyncs %d, late %d, dup %d, srtp errors %d' %
      (recv['rtp_packets'], recv['udp_rx'], recv['udp_dropped'], recv['nacks_sent'], pion['rtcp_nack_packets'],
       pion['rtcp_nack_seqs'], recv['jit_dropped'], recv['jit_resyncs'], recv['jit_late'], recv['jit_dup'],
       recv['srtp_errors']))
print('rtcp  : PLI sent %d (pion %d), keyframe requests %d (pion jumps %d), REMB %d (pion %d, %.0f bps), RR %d (pion %d, '
      'ssrc ok %s, max fraction lost %d/256)' %
      (recv['pli_sent'], pion['rtcp_pli'], recv['keyframe_requests'], pion['keyframe_jumps'], recv['remb_sent'],
       pion['rtcp_remb'], pion['remb_bps_last'], recv['rr_sent'], pion['rtcp_rr'], pion['rr_ssrc_matches_video'],
       pion['rr_fraction_lost_max']))
if recv['srtp_errors']:
    fails.append('srtp errors')
if loss > 0 and (recv['nacks_sent'] == 0 or pion['rtcp_nack_seqs'] == 0):
    fails.append('no NACK under loss')
if pion['rtcp_remb'] == 0 or pion['rtcp_rr'] == 0 or not pion['rr_ssrc_matches_video']:
    fails.append('RR/REMB missing')
if pion['rtcp_pli'] == 0:
    fails.append('no PLI seen')

# ---- datachannels
ch = sorted(pion['channels'] or [])
print('dc    : channels %s' % ch)
print('dc    : HandshakeAck flow %s, startup targets %d, control %s' %
      (pion['handshake_seen'], len(pion['startup_targets'] or []), pion['control_messages']))
want_ch = ['chat/chatV1/6', 'control/controlV1/0', 'input/1.0/2', 'message/messageV1/4']
if ch != want_ch:
    fails.append('channel set %s != %s' % (ch, want_ch))
if len(pion['startup_targets'] or []) != 6:
    fails.append('startup messages')
for m in ('authorizationRequest', 'gamepadChanged', 'userRequestedResolutionUpdate', 'videoKeyframeRequested'):
    if not pion['control_messages'].get(m):
        fails.append('control message %s missing' % m)
print('input : %d reports (metadata %d), first pad seq %d, last seq %d, seq gaps %d, bad %d, A toggles %d, LX %d..%d, '
      'receiver sent %d / fail %d' %
      (pion['input_packets'], pion['input_metadata'], pion['input_first_pad_seq'], pion['input_last_seq'],
       pion['input_seq_gaps'], pion['input_bad_len'], pion['input_a_toggles'], pion['input_min_lx'],
       pion['input_max_lx'], recv['input_sent'], recv['input_fail']))
if pion['input_seq_gaps'] or pion['input_bad_len'] or pion['input_metadata'] != 1 or pion['input_first_pad_seq'] != 1 \
        or pion['input_a_toggles'] < 4:
    fails.append('input stream')
print('rtt   : chat ping/pong %d/%d, avg %.3f ms, max %.3f ms' % (recv['pongs'], recv['pings'], recv['rtt_avg_ms'],
                                                                  recv['rtt_max_ms']))
if recv['pings'] == 0 or recv['pongs'] < recv['pings'] - 2:
    fails.append('datachannel round trip')
if recv['state'] != 'streaming':
    fails.append('final state %s (%s)' % (recv['state'], recv['fail_reason']))
mp = os.path.join(d, 'recv.mem')
if os.path.exists(mp):
    m = json.load(open(mp))
    print('memory: stack high-water %d B, heap peak %d B above baseline (WebRTC stack + jitter pool + AU buffers)' %
          (m['stack_peak_bytes'], m['heap_peak_bytes'] - m['heap_base_bytes']))
tp = os.path.join(d, 'recv.time')
if os.path.exists(tp):
    u, sy, e = (float(v) for v in open(tp).read().split()[:3])
    print('cpu   : receiver process user %.2f s + sys %.2f s over %.1f s wall (%.1f%% user of one core, incl. the '
          '0.5 ms poll loop)' % (u, sy, e, 100 * u / e))
print('VERDICT: %s' % ('PASS' if not fails else 'FAIL: ' + '; '.join(fails)))
sys.exit(1 if fails else 0)
