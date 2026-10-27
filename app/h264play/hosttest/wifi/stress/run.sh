#!/bin/sh
# orbita v0.21.8 — estresse do caminho de dados do Wi-Fi (EHCI falso + xhs + core + lwIP + par TCP).
# Uso: hosttest/wifi/stress/run.sh [pacotes]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../../.." && pwd)
LW=${LWIP_SRC:-$HOME/libxenon/libxenon/drivers/lwip}
OUT=${TMPDIR:-/tmp}/xc360-wifi-stress
mkdir -p "$OUT"
N=${1:-120000}
W="$APP/source/wifi"
INC="-I$HERE -I$W -I$LW/include -I$LW/include/ipv4 -I$HERE/lwipopts"
LWSRC="$LW/core/def.c $LW/core/init.c $LW/core/mem.c $LW/core/memp.c $LW/core/netif.c $LW/core/pbuf.c $LW/core/raw.c \
 $LW/core/stats.c $LW/core/timers.c $LW/core/tcp.c $LW/core/tcp_in.c $LW/core/tcp_out.c $LW/core/udp.c $LW/core/dhcp.c \
 $LW/core/ipv4/icmp.c $LW/core/ipv4/inet.c $LW/core/ipv4/inet_chksum.c $LW/core/ipv4/ip.c $LW/core/ipv4/ip_addr.c \
 $LW/core/ipv4/ip_frag.c $LW/netif/etharp.c"
SRC="$HERE/stress.c $W/xw_ehs_xenon.c $W/xw_core.c $W/mwx_proto.c $W/xw_wpa.c $W/xw_rxwatch.c"
MB=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
CF="-O1 -g -DXHS_HOSTTEST -Wno-address-of-packed-member -I$MB/include -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\""
gcc $CF -fsanitize=address,undefined -fno-sanitize=alignment $INC -o "$OUT/stress_det" $SRC $LWSRC "$MB/libmbedtls-xc360.a" -lpthread
gcc $CF $INC -o "$OUT/stress_thr" $SRC $LWSRC "$MB/libmbedtls-xc360.a" -lpthread
rc=0
"$OUT/stress_det" det "$N" 7 || rc=1
"$OUT/stress_thr" thread "$N" 11 || rc=1
[ $rc -eq 0 ] && echo "WIFI STRESS PASSED" || echo "WIFI STRESS FAILED"
exit $rc
