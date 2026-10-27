#!/bin/sh
# orbita v0.21.0 — testes do host do Wi-Fi (source/wifi). Uso: hosttest/wifi/run.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
OUT=${TMPDIR:-/tmp}/xc360-wifi-hosttest
MBED=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
mkdir -p "$OUT"
if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
W="$APP/source/wifi"
CF="-DXWD_HOSTTEST -O1 -g -Wall -Wextra -Wno-unused-parameter -I$APP/source -I$W -I$APP/source/beta -I$MBED/include -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\""
gcc $CF -fsanitize=address,undefined -o "$OUT/wifi_unit" "$HERE/wifi_unit.c" "$W/mwx_proto.c" "$W/xw_core.c" \
    "$W/xw_wpa.c" "$W/xw_store.c" "$W/xw_boot.c" "$APP/source/xc_wdog.c" "$APP/source/beta/xc_keypad.c" "$MBED/libmbedtls-xc360.a"
rc=0
"$OUT/wifi_unit" || rc=1
# PTK/PMK conferidas com uma implementação independente (Python hashlib/hmac)
python3 -I - "$OUT/wifi_unit" <<'PY' || rc=1
import hashlib, hmac, os, subprocess, sys
exe = sys.argv[1]
bad = 0
for i in range(20):
    pw = os.urandom(6).hex() + "aB!"
    ssid = os.urandom(1 + i % 16).hex()[: 1 + i]
    want = hashlib.pbkdf2_hmac("sha1", pw.encode(), ssid.encode(), 4096, 32).hex()
    got = subprocess.check_output([exe, "pmk", pw, ssid]).decode().strip()
    bad += want != got
    pmk, aa, spa, an, sn = os.urandom(32), os.urandom(6), os.urandom(6), os.urandom(32), os.urandom(32)
    d = min(aa, spa) + max(aa, spa) + min(an, sn) + max(an, sn)
    out = b""
    c = 0
    while len(out) < 48:
        out += hmac.new(pmk, b"Pairwise key expansion\0" + d + bytes([c]), hashlib.sha1).digest()
        c += 1
    got = subprocess.check_output([exe, "ptk", pmk.hex(), aa.hex(), spa.hex(), an.hex(), sn.hex()]).decode().strip()
    bad += out[:48].hex() != got
print("python: PMK/PTK x20 %s" % ("OK" if not bad else "%d DIFERENTES" % bad))
sys.exit(1 if bad else 0)
PY
# v0.21.8: estresse do caminho de dados (EHCI HS falso + xw + lwIP, >100k RX). XC360_STRESS_RX muda o alvo.
sh "$HERE/stress/run.sh" "${XC360_STRESS_RX:-120000}" || rc=1
[ $rc -eq 0 ] && echo "WIFI HOST TESTS PASSED" || echo "WIFI HOST TESTS FAILED"
exit $rc
