#!/bin/bash
# XCloud360 — source this file to configure devkitxenon toolchain PATH.
# Usage: source scripts/env.sh
#
# Assumes toolchain installed at ~/devkitxenon (ADR-001). Override with
# DEVKITXENON env var before sourcing.

export DEVKITXENON="${DEVKITXENON:-$HOME/devkitxenon}"

if [ ! -d "$DEVKITXENON" ]; then
    echo "[env] WARN: $DEVKITXENON does not exist. Bootstrap the toolchain first:" >&2
    echo "      cd ~/libxenon/toolchain && FREE60_DOCKER_BUILD=true PREFIX=\$HOME/devkitxenon ./build-xenon-toolchain toolchain" >&2
    return 1 2>/dev/null || exit 1
fi

case ":$PATH:" in
    *":$DEVKITXENON/bin:"*) ;;
    *) export PATH="$DEVKITXENON/bin:$DEVKITXENON/usr/bin:$PATH" ;;
esac

echo "[env] DEVKITXENON=$DEVKITXENON"
if command -v xenon-gcc >/dev/null 2>&1; then
    echo "[env] xenon-gcc: $(xenon-gcc --version | head -1)"
else
    echo "[env] xenon-gcc NOT FOUND in PATH (toolchain build may still be running)"
fi
