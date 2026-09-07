#!/bin/bash
# xcloud-capture — bootstrap Green-NX PC CLI as an external subprocess dep.
# Idempotent: safe to re-run.

set -euo pipefail

GNX_DIR="$HOME/green-nx"
GNX_BIN="$GNX_DIR/build/pc/greennx"

echo "[setup] Green-NX PC CLI at $GNX_BIN"

# 1) Clone if missing.
if [ ! -d "$GNX_DIR" ]; then
    echo "[setup] cloning rmrf404/green-nx (GPL-3) to $GNX_DIR ..."
    git clone --depth 1 https://github.com/rmrf404/green-nx.git "$GNX_DIR"
else
    echo "[setup] $GNX_DIR already exists, skipping clone."
fi

# 2) Verify libcurl-dev is installed.
if ! pkg-config --exists libcurl 2>/dev/null; then
    echo "[setup] ERROR: libcurl development headers missing." >&2
    echo "[setup] Install with: sudo apt-get install -y libcurl4-openssl-dev" >&2
    exit 2
fi

# 3) Build PC target if binary is missing or older than source.
NEEDS_BUILD=0
if [ ! -x "$GNX_BIN" ]; then
    NEEDS_BUILD=1
elif find "$GNX_DIR/src/core" "$GNX_DIR/src/cli" -newer "$GNX_BIN" 2>/dev/null | grep -q .; then
    NEEDS_BUILD=1
fi
if [ "$NEEDS_BUILD" = "1" ]; then
    echo "[setup] building Green-NX PC (Makefile.pc)..."
    (cd "$GNX_DIR" && make -f Makefile.pc)
else
    echo "[setup] $GNX_BIN up to date."
fi

# 4) Smoke test — usage banner should print. greennx exits 2 on no-args so
#    disable pipefail locally for the check.
set +o pipefail
BANNER="$("$GNX_BIN" 2>&1 | head -1)"
set -o pipefail
if [[ "$BANNER" == "usage: greennx"* ]]; then
    SIZE="$(stat -c '%s' "$GNX_BIN")"
    echo "[setup] OK: greennx runs ($SIZE bytes)"
else
    echo "[setup] ERROR: greennx did not print expected usage banner" >&2
    echo "[setup] got: $BANNER" >&2
    exit 3
fi

# 5) Token file check.
if [ -f "$HOME/.green-nx-tokens.json" ]; then
    echo "[setup] token store exists ($HOME/.green-nx-tokens.json)"
    echo "[setup] --> next: tools/xcloud-capture/games.sh"
else
    echo "[setup] no token store yet ($HOME/.green-nx-tokens.json)"
    echo "[setup] --> next: tools/xcloud-capture/login.sh"
fi
