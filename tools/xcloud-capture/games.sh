#!/bin/bash
# xcloud-capture — dump the account's playable xCloud library.
# Also saves the game -> title-id map for later capture-signaling.sh runs.

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

OUT_DIR="$(dirname "$0")/output"
mkdir -p "$OUT_DIR"
TS=$(date +%Y%m%d-%H%M%S)
LIST="$OUT_DIR/games-$TS.txt"
IDS="$OUT_DIR/ids-$TS.txt"

echo "[games] fetching playable library..."
"$GNX_BIN" games | tee "$LIST"
echo ""
echo "[games] fetching game ID mapping..."
"$GNX_BIN" ids | tee "$IDS"

echo ""
echo "[games] wrote:"
echo "  $LIST"
echo "  $IDS"
echo ""
echo "[games] pick a title id from ids-*.txt; then:"
echo "  tools/xcloud-capture/capture-signaling.sh <TITLE_ID> [--ads]"
