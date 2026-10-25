#!/usr/bin/env python3
"""XCloud360 stage 5 — multi-controller part of a standalone E2E session.

The test sends pad 0 from the start and pad 1 from t=2 s (24-byte frames ->
xn_pad_frame -> xcw_set_gamepad). pion must have seen gamepadChanged(1, added)
and input reports for both indices, one shared contiguous sequence.
usage: check_pads.py <session dir> [induced loss]  (loss > 0: NACK RTT samples required)
"""
import json, os, sys

d = sys.argv[1]
loss = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
p = json.load(open(os.path.join(d, 'pion.json')))
r = json.load(open(os.path.join(d, 'recv.json')))
reps = p.get('input_pad_reports') or {}
ann = p.get('gamepad_changed_idx') or []
fails = []
print('pads  : reports per index %s, gamepadChanged(added) %s, receiver announced %d, ordered %s, '
      'rtt(nack) samples %d, rtp lost %d/%d, keyframe requests %d (pion jumps %d)' %
      (reps, ann, r['pads_announced'], p.get('pad_announced_before_reports'), r['rtt_samples'], r['rtp_lost'],
       r['rtp_expected'], r['keyframe_requests'], p['keyframe_jumps']))
if reps.get('0', 0) < 50:
    fails.append('pad 0 reports')
if reps.get('1', 0) < 20:
    fails.append('pad 1 reports')
if 0 not in ann or 1 not in ann:
    fails.append('gamepadChanged for 0 and 1')
if p['input_seq_gaps']:
    fails.append('sequence gaps')
if loss > 0 and r["rtt_samples"] == 0:
    fails.append("no NACK RTT sample under loss")
if r["keyframe_requests"] < 1:
    fails.append('no keyframe request')
print('PADS VERDICT: %s' % ('PASS' if not fails else 'FAIL: ' + '; '.join(fails)))
sys.exit(1 if fails else 0)
