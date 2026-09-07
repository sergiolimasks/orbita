#!/bin/bash
# xcloud-capture — Microsoft device-code sign-in via Green-NX CLI.
#
# The device code is printed to stdout. Open the URL in any browser,
# type the code, approve the request. This script blocks until the
# Microsoft server signals authorization or the code expires.
#
# Tokens are cached in $HOME/.green-nx-tokens.json.

set -euo pipefail

GNX_BIN="$HOME/green-nx/build/pc/greennx"
if [ ! -x "$GNX_BIN" ]; then
    echo "ERROR: $GNX_BIN not found. Run tools/xcloud-capture/setup.sh first." >&2
    exit 2
fi

echo "[login] running: $GNX_BIN login"
echo "[login] you will need to visit microsoft.com/link and enter a code."
echo ""
exec "$GNX_BIN" login
