#!/bin/sh
# Full suite (diagnostic build, GIP_DEBUG=1) + production build (GIP_DEBUG=0).
HERE=$(cd "$(dirname "$0")" && pwd)
GIPFLAGS="-DGIP_DEBUG=1" sh "$HERE/run.sh" "$@" || exit 1
GIPFLAGS="-DGIP_DEBUG=0" sh "$HERE/run.sh" "$@" || exit 1
