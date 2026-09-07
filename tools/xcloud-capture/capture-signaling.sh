#!/bin/bash
# xcloud-capture — 5.3 signaling capture.
# Runs `greennx stream-test <TITLE_ID> [--ads]` and tees every line to a
# per-run output directory. From that trace, later stages will extract the
# SDP offer/answer, ICE candidates, and endpoint metadata.
#
# For ad-supported catalog (recommended for early captures):
#   ./capture-signaling.sh FORTNITE --ads

set -euo pipefail

GNX_BIN="$HOME/green-nx/build/pc/greennx"
if [ ! -x "$GNX_BIN" ]; then
    echo "ERROR: $GNX_BIN not found. Run tools/xcloud-capture/setup.sh first." >&2
    exit 2
fi
if [ ! -f "$HOME/.green-nx-tokens.json" ]; then
    echo "ERROR: no token store. Run tools/xcloud-capture/login.sh first." >&2
    exit 3
fi

TITLE_ID="${1:-}"
if [ -z "$TITLE_ID" ]; then
    echo "usage: $0 <TITLE_ID> [--ads]" >&2
    echo "run tools/xcloud-capture/games.sh first to see title ids." >&2
    exit 1
fi
shift
ADS_FLAG=""
if [ "${1:-}" = "--ads" ]; then
    ADS_FLAG="--ads"
fi

OUT_ROOT="$(dirname "$0")/output"
TS=$(date +%Y%m%d-%H%M%S)
OUT_DIR="$OUT_ROOT/$TITLE_ID-$TS"
mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/greennx-stream-test.log"

echo "[cap-sig] title:    $TITLE_ID"
echo "[cap-sig] ads flag: ${ADS_FLAG:-none}"
echo "[cap-sig] output:   $OUT_DIR"
echo "[cap-sig] log:      $LOG"
echo ""
echo "[cap-sig] --- greennx stream-test $TITLE_ID $ADS_FLAG ---"

"$GNX_BIN" stream-test "$TITLE_ID" $ADS_FLAG 2>&1 | tee "$LOG"

echo ""
echo "[cap-sig] done. next: extract SDP + ICE from $LOG"
echo "[cap-sig] (extractors will land in analyze/ once we tighten this pass)"
