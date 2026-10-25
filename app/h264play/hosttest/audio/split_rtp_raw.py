#!/usr/bin/env python3
"""Split capture-rtp's old audio.opus.raw (whole RTP packets written back to
back, no length prefix) into packets, by chaining headers: V=2, PT=111, same
SSRC, seq+1. Prints the Opus TOC census and writes a .pkts file
(u16 BE len, u16 BE seq, u32 BE ts, payload) usable by the host tests.

usage: split_rtp_raw.py audio.opus.raw out.pkts
"""
import sys, struct, collections

data = open(sys.argv[1], 'rb').read()

def hdr_ok(p, ssrc=None, seq=None):
    if p + 12 > len(data): return False
    b0, b1 = data[p], data[p + 1]
    if (b0 >> 6) != 2 or (b1 & 0x7f) != 111: return False
    s, = struct.unpack('>H', data[p + 2:p + 4])
    c, = struct.unpack('>I', data[p + 8:p + 12])
    if ssrc is not None and c != ssrc: return False
    if seq is not None and s != (seq + 1) & 0xffff: return False
    return True

def payload_off(p):
    b0 = data[p]
    cc = b0 & 0x0f
    off = p + 12 + 4 * cc
    if b0 & 0x10:
        ln, = struct.unpack('>H', data[off + 2:off + 4])
        off += 4 + 4 * ln
    return off

pkts = []
p = 0
while p < len(data) and not hdr_ok(p): p += 1
while p < len(data):
    seq, = struct.unpack('>H', data[p + 2:p + 4])
    ts, = struct.unpack('>I', data[p + 4:p + 8])
    ssrc, = struct.unpack('>I', data[p + 8:p + 12])
    q = payload_off(p) + 1
    while q < len(data) and not hdr_ok(q, ssrc, seq): q += 1
    end = q if q < len(data) else len(data)
    pl = data[payload_off(p):end]
    if data[p] & 0x20 and pl:  # padding
        pl = pl[:-pl[-1]]
    pkts.append((seq, ts, pl, data[p] & 0x10))
    p = q

cfgs = collections.Counter()
modes = {range(0, 12): 'SILK', range(12, 16): 'HYBRID', range(16, 32): 'CELT'}
for seq, ts, pl, x in pkts:
    toc = pl[0]
    c = toc >> 3
    m = [v for k, v in modes.items() if c in k][0]
    cfgs[(m, c, 'stereo' if toc & 4 else 'mono', toc & 3)] += 1
sizes = [len(pl) for _, _, pl, _ in pkts]
dts = collections.Counter((pkts[i + 1][1] - pkts[i][1]) & 0xffffffff for i in range(len(pkts) - 1))
print('packets', len(pkts), 'ext-hdr', sum(1 for p in pkts if p[3]))
print('toc census (mode, config, ch, code):', dict(cfgs))
print('payload bytes min/avg/max', min(sizes), sum(sizes) / len(sizes), max(sizes))
print('ts deltas', dict(dts.most_common(5)))
print('approx kbps', sum(sizes) * 8 / (len(pkts) * 0.02) / 1000)
with open(sys.argv[2], 'wb') as f:
    for seq, ts, pl, _ in pkts:
        f.write(struct.pack('>HHI', len(pl), seq, ts) + pl)
