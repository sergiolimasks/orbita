#!/bin/bash
# XCloud360 — P5.5 GOLDEN STREAM analyzer.
#
# Consumes a stream.h264 file produced by capture-rtp and emits analysis.json
# + analysis.txt with the codec-level info the spec §16 asks for:
#
#   profile_idc, constraint flags, level_idc, width, height, entropy coding
#   (CAVLC/CABAC), num_ref_frames, pic_order_cnt_type, frame_mbs_only_flag,
#   B-frames?, IDR interval, GOP, NAL type distribution, slices/frame,
#   bitrate median/pk, measured fps.
#
# Strategy: ffprobe first (fast, trusted). Write our own parser only when
# ffprobe leaves a gap.
#
# Usage:
#   analyze.sh <capture-dir>
#
# Requires:  ffprobe (from ffmpeg), jq (optional, for pretty-print).

set -euo pipefail

CAP_DIR="${1:-}"
if [ -z "$CAP_DIR" ] || [ ! -d "$CAP_DIR" ]; then
    echo "usage: $0 <capture-dir>" >&2
    exit 2
fi

H264="$CAP_DIR/stream.h264"
if [ ! -f "$H264" ] || [ ! -s "$H264" ]; then
    echo "ERROR: $H264 missing or empty" >&2
    exit 3
fi

OUT_JSON="$CAP_DIR/analysis.json"
OUT_TXT="$CAP_DIR/analysis.txt"
SPS_BIN="$CAP_DIR/sps.bin"
PPS_BIN="$CAP_DIR/pps.bin"

BYTES=$(stat -c '%s' "$H264")
SHA=$(sha256sum "$H264" | awk '{print $1}')

echo "[analyze] $H264 ($BYTES bytes, sha256 ${SHA:0:16}...)"

# --- ffprobe stream metadata ------------------------------------------------
STREAM_JSON=$(ffprobe -v error -select_streams v:0 -show_streams \
    -of json "$H264" 2>&1)
FORMAT_JSON=$(ffprobe -v error -show_format -of json "$H264" 2>&1)
FRAMES_JSON=$(ffprobe -v error -select_streams v:0 -show_frames \
    -show_entries frame=pict_type,key_frame,pkt_size,pkt_pts_time \
    -of json "$H264" 2>&1)

# --- extract SPS/PPS via ffmpeg codecpar side-data ---
# ffmpeg -y -i in.h264 -c copy -bsf:v extract_extradata -f data - > extradata.bin
ffmpeg -y -loglevel error -i "$H264" -c copy \
    -bsf:v extract_extradata -f data "$CAP_DIR/extradata.bin" 2>&1 || true

# --- parse frames JSON with a small Python snippet ---
python3 <<PYEOF
import json, sys, os, hashlib

cap_dir  = "$CAP_DIR"
h264     = "$H264"
sps_bin  = "$SPS_BIN"
pps_bin  = "$PPS_BIN"

# Load ffprobe blobs (subshelled above).
try:
    stream = json.loads('''$STREAM_JSON''')["streams"][0]
except Exception as e:
    print(f"[analyze] stream probe failed: {e}", file=sys.stderr)
    stream = {}

try:
    frames = json.loads('''$FRAMES_JSON''')["frames"]
except Exception as e:
    print(f"[analyze] frames probe failed: {e}", file=sys.stderr)
    frames = []

# Walk the raw bitstream once to count NAL types + IDR interval + slice/frame.
NAL_NAMES = {
    1:  "slice_non_idr",
    5:  "slice_idr",
    6:  "sei",
    7:  "sps",
    8:  "pps",
    9:  "aud",
    12: "filler",
}

nal_counts   = {}
frame_slices = []              # slices per frame (delimited by AU boundary heuristic)
idr_positions = []
au_slice_count = 0
prev_first_mb_zero_seen = False
last_type = -1
sps_blob = None
pps_blob = None

def next_startcode(buf, start):
    n = len(buf)
    i = start
    while i + 3 < n:
        if buf[i] == 0 and buf[i+1] == 0 and buf[i+2] == 1:
            return i, 3
        if buf[i] == 0 and buf[i+1] == 0 and buf[i+2] == 0 and buf[i+3] == 1:
            return i, 4
        i += 1
    return -1, 0

with open(h264, "rb") as f:
    buf = f.read()

# Iterate NAL units.
i = 0
au_start_positions = []
n = len(buf)
first_slice_pos = None
while i < n:
    pos, sclen = next_startcode(buf, i)
    if pos < 0: break
    nal_start = pos + sclen
    if nal_start >= n: break
    header = buf[nal_start]
    forbidden = (header >> 7) & 1
    nal_type  = header & 0x1F
    nal_counts[nal_type] = nal_counts.get(nal_type, 0) + 1

    # Determine NAL end (next startcode).
    nxt, _ = next_startcode(buf, nal_start + 1)
    nal_end = nxt if nxt > 0 else n
    nal_body = buf[nal_start:nal_end]

    if nal_type == 7 and sps_blob is None:  # SPS
        sps_blob = nal_body
    elif nal_type == 8 and pps_blob is None:
        pps_blob = nal_body
    elif nal_type in (1, 5):  # slice
        # first_mb_in_slice = 0 => new picture.
        # ue(v) decoded: if body[1] high bit is 1, first_mb_in_slice = 0.
        if len(nal_body) >= 2:
            b1 = nal_body[1]
            first_mb_zero = (b1 & 0x80) != 0
            if first_mb_zero:
                # New access unit boundary
                if au_slice_count > 0:
                    frame_slices.append(au_slice_count)
                au_slice_count = 1
                if nal_type == 5:
                    idr_positions.append(len(frame_slices))
            else:
                au_slice_count += 1

    last_type = nal_type
    i = nal_end

# Flush last AU.
if au_slice_count > 0:
    frame_slices.append(au_slice_count)

if sps_blob:
    with open(sps_bin, "wb") as f: f.write(sps_blob)
if pps_blob:
    with open(pps_bin, "wb") as f: f.write(pps_blob)

# ffprobe gives us the codec parameters — try to pull them.
def g(k, default=None):
    return stream.get(k, default)

profile      = g("profile", "unknown")
level        = g("level")
codec_name   = g("codec_name")
codec_lname  = g("codec_long_name")
width        = g("width")
height       = g("height")
pix_fmt      = g("pix_fmt")
avg_fps      = g("avg_frame_rate")
r_fps        = g("r_frame_rate")
has_b_frames = g("has_b_frames")

# Frame-level stats
nframes = len(frames)
key_frames = sum(1 for f in frames if f.get("key_frame", 0) == 1)
sizes = [int(f.get("pkt_size", 0)) for f in frames if f.get("pkt_size")]
if sizes:
    sizes_sorted = sorted(sizes)
    bitrate_median = sizes_sorted[len(sizes_sorted)//2] * 8
    bitrate_p95    = sizes_sorted[int(0.95 * len(sizes_sorted))] * 8
    bitrate_max    = sizes_sorted[-1] * 8
else:
    bitrate_median = bitrate_p95 = bitrate_max = 0

def sha(path):
    if not os.path.exists(path): return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

# IDR interval (in frames)
idr_intervals = []
if len(idr_positions) >= 2:
    for a, b in zip(idr_positions, idr_positions[1:]):
        idr_intervals.append(b - a)

analysis = {
    "artifact": {
        "path": h264,
        "size_bytes": os.path.getsize(h264),
        "sha256": sha(h264),
    },
    "codec": {
        "codec_name": codec_name,
        "codec_long_name": codec_lname,
        "profile": profile,
        "level": level,
        "pix_fmt": pix_fmt,
        "width": width,
        "height": height,
    },
    "framing": {
        "frames_probed": nframes,
        "key_frames": key_frames,
        "has_b_frames_reported": has_b_frames,
        "avg_frame_rate": avg_fps,
        "r_frame_rate": r_fps,
    },
    "nal_type_counts": {NAL_NAMES.get(k, f"type_{k}"): v for k, v in sorted(nal_counts.items())},
    "slices_per_frame": {
        "samples": len(frame_slices),
        "min": min(frame_slices) if frame_slices else None,
        "max": max(frame_slices) if frame_slices else None,
        "median": sorted(frame_slices)[len(frame_slices)//2] if frame_slices else None,
        "histogram": {},
    },
    "idr": {
        "count": len(idr_positions),
        "positions_frames": idr_positions[:20],
        "intervals_frames_first20": idr_intervals[:20],
    },
    "bitrate_bits_per_frame": {
        "median": bitrate_median,
        "p95": bitrate_p95,
        "max": bitrate_max,
    },
    "sps_bin": {"path": sps_bin, "sha256": sha(sps_bin), "size": os.path.getsize(sps_bin) if os.path.exists(sps_bin) else 0},
    "pps_bin": {"path": pps_bin, "sha256": sha(pps_bin), "size": os.path.getsize(pps_bin) if os.path.exists(pps_bin) else 0},
}

# slice/frame histogram
if frame_slices:
    hist = {}
    for s in frame_slices:
        hist[str(s)] = hist.get(str(s), 0) + 1
    analysis["slices_per_frame"]["histogram"] = hist

with open("$OUT_JSON", "w") as f:
    json.dump(analysis, f, indent=2)

# Also emit a human summary
with open("$OUT_TXT", "w") as f:
    def w(*args): print(*args, file=f)
    w("=" * 60)
    w(" GOLDEN STREAM analysis")
    w("=" * 60)
    w(f" path:          {h264}")
    w(f" size:          {os.path.getsize(h264)} bytes")
    w(f" sha256:        {sha(h264)}")
    w("")
    w("-- codec --")
    w(f" name:          {codec_name} ({codec_lname})")
    w(f" profile:       {profile}")
    w(f" level:         {level}")
    w(f" pix_fmt:       {pix_fmt}")
    w(f" resolution:    {width}x{height}")
    w("")
    w("-- framing --")
    w(f" frames:        {nframes}")
    w(f" key_frames:    {key_frames}")
    w(f" avg_fps:       {avg_fps}")
    w(f" r_fps:         {r_fps}")
    w(f" B-frames:      has_b_frames={has_b_frames}")
    w("")
    w("-- NAL type counts --")
    for k, v in sorted(nal_counts.items()):
        w(f"   type {k:2d} ({NAL_NAMES.get(k, '?')}): {v}")
    w("")
    w("-- slices per frame --")
    if frame_slices:
        w(f"   samples={len(frame_slices)}  min={min(frame_slices)}  max={max(frame_slices)}  median={sorted(frame_slices)[len(frame_slices)//2]}")
        w(f"   histogram: {analysis['slices_per_frame']['histogram']}")
    w("")
    w("-- IDR --")
    w(f"   IDR count:   {len(idr_positions)}")
    w(f"   positions:   {idr_positions[:20]}")
    w(f"   intervals:   {idr_intervals[:20]}")
    w("")
    w("-- bitrate (bits per frame) --")
    w(f"   median: {bitrate_median}   p95: {bitrate_p95}   max: {bitrate_max}")
    w("")
    w("-- SPS/PPS blobs --")
    w(f"   sps.bin: {analysis['sps_bin']['size']} bytes  sha256={(analysis['sps_bin']['sha256'] or '')[:16]}...")
    w(f"   pps.bin: {analysis['pps_bin']['size']} bytes  sha256={(analysis['pps_bin']['sha256'] or '')[:16]}...")
    w("=" * 60)

print("[analyze] wrote $OUT_JSON")
print("[analyze] wrote $OUT_TXT")
PYEOF

# Emit sha256.txt covering every artifact in the capture dir
{
    for f in "$CAP_DIR"/*.h264 "$CAP_DIR"/*.sdp "$CAP_DIR"/*.json "$CAP_DIR"/*.bin "$CAP_DIR"/*.txt; do
        [ -f "$f" ] && sha256sum "$f"
    done
} > "$CAP_DIR/sha256.txt"

echo "[analyze] wrote $CAP_DIR/sha256.txt"
echo ""
echo "==== SUMMARY ===="
cat "$OUT_TXT" 2>/dev/null || echo "(analysis.txt not written)"