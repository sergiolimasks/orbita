#!/bin/sh
# XCloud360 — testes do host do xc_update (source/update, UPDATE.md).
# Uso: app/h264play/hosttest/update/run_update.sh [unit|live|all]   (padrão unit)
#   unit: sem rede. Chave ECDSA de TESTE gerada aqui (nunca a de release),
#         fetch falso, pendrive = pasta, falhas injetadas; ASan+UBSan.
#   live: contra a ponte de verdade (https://x360.pistatrack.com, ?env=test)
#         com o NOSSO HTTPS; precisa de um release de teste publicado antes
#         (tools/release.py publish --test ...; ver UPDATE.md). Usa a chave
#         pública EMBUTIDA (a assinatura tem que ser a de release).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source/login"
UPD="$APP/source/update"
OUT=${TMPDIR:-/tmp}/xc360-update-hosttest
MBED=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
WHAT=${1:-unit}
mkdir -p "$OUT"

if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
EPOCH=$(date +%s)
CF="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I$SRC -I$MBED/include \
    -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=${EPOCH}LL"
CORE="$UPD/xc_update.c $SRC/cjson/cJSON.c $HERE/xcu_port_posix.c $HERE/../login/port_posix.c $SRC/dns_msg.c"
NET="$UPD/xcu_net.c $SRC/https.c $SRC/http_parse.c $SRC/ca_roots.c"

rc=0
if [ "$WHAT" = unit ] || [ "$WHAT" = all ]; then
    FX="$OUT/fixtures"
    rm -rf "$FX"; mkdir -p "$FX"
    openssl ecparam -name prime256v1 -genkey -noout -out "$FX/test.pem" 2>/dev/null
    openssl ecparam -name prime256v1 -genkey -noout -out "$FX/other.pem" 2>/dev/null
    openssl ec -in "$FX/test.pem" -pubout -outform DER -out "$FX/pub.der" 2>/dev/null
    python3 - "$REPO/tools" "$FX" <<'PY'
import sys, os, hashlib, json
sys.path.insert(0, sys.argv[1]); fx = sys.argv[2]
import release as R
new = b"\x7fELF\x01\x02\x01" + os.urandom(1300 * 1024 + 77)
old = b"\x7fELF\x01\x02\x01" + os.urandom(300 * 1024)
open(fx + "/new.elf", "wb").write(new); open(fx + "/old.elf", "wb").write(old)
def man(ver, ch, data, notes="Correções do áudio e menu novo"):
    return {"format": 1, "channel": ch, "version": ver, "size": len(new),
            "sha256": hashlib.sha256(data).hexdigest(), "url": "/v1/update/%s/xenon.elf" % ver, "notes": notes}
k, o = fx + "/test.pem", fx + "/other.pem"
w = lambda n, m: open(fx + "/" + n, "wb").write(R.manifest_bytes(m))
g = R.sign(man("0.21.0-beta.1", "beta", new), k); w("good.json", g)
b = dict(g); b["notes"] = "Correções do áudio e menu novo!"; w("badsig.json", b)
w("otherkey.json", R.sign(man("0.21.0-beta.1", "beta", new), o))
w("wrongsha.json", R.sign(man("0.21.0-beta.1", "beta", old), k))
w("stable_old.json", R.sign(man("0.19.0", "stable", new), k))
assert R.verify(g, k) and not R.verify(b, k)
# v0.20.3: atualização obrigatória ("mandatory" + "sig2")
mm = man("0.21.0-beta.1", "beta", new); mm["mandatory"] = True
w("mand.json", R.sign(mm, k))
mo = man("0.21.0-beta.1", "beta", new); mo["mandatory"] = False
so = R.sign(mo, k); w("opt.json", so)
assert R.verify(so, k) and not R.is_mandatory(so)
t = dict(R.sign(mm, k)); t["mandatory"] = False; w("opt_tampered.json", t)   # sig2 de "true"
assert not R.verify(t, k)
ns = dict(so); del ns["sig2"]; w("opt_nosig2.json", ns)
# promote preserva o campo: reassina como stable mantendo "mandatory"
pr = {x: y for x, y in so.items() if x not in ("sig", "sig2")}; pr["channel"] = "stable"
ps = R.sign(pr, k); assert R.verify(ps, k) and ps["mandatory"] is False
# app antigo: só o "sig" de sempre (bytes legados) continua valendo
legacy = {x: y for x, y in so.items() if x not in ("mandatory", "sig2")}
assert R.verify(legacy, k)
PY
    gcc $CF -fsanitize=address,undefined -o "$OUT/update_unit" "$HERE/update_unit.c" $CORE \
        "$MBED/libmbedtls-xc360.a" -lpthread
    "$OUT/update_unit" "$FX" "$OUT/usb" || rc=1
fi
if [ "$WHAT" = live ] || [ "$WHAT" = all ]; then
    gcc $CF -o "$OUT/update_live" "$HERE/update_live.c" $CORE $NET "$MBED/libmbedtls-xc360.a" -lpthread
    echo "--- live (hora do host)"
    "$OUT/update_live" "$OUT/live" "${2:-promoted}" || rc=1
    echo "--- live (sem hora: bootstrap pelo Date, como no Xbox)"
    XC_TIME_UNKNOWN=1 "$OUT/update_live" "$OUT/live" "${2:-promoted}" || rc=1
fi
[ $rc -eq 0 ] && echo "UPDATE HOST TESTS PASSED" || echo "UPDATE HOST TESTS FAILED"
exit $rc
