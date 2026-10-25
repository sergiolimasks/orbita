#!/usr/bin/env bash
# XCloud360 v0.17 — mbedTLS 3.4.0 como lib estática pro Xbox (ou host).
# Uso: deps/build-mbedtls-xenon.sh            -> $HOME/mbedtls-xenon/libmbedtls-xc360.a
#      HOST=1 OUT=/tmp/x deps/build-mbedtls-xenon.sh  -> versão gcc do host (hosttest)
# Config: deps/mbedtls-xenon/xc360_mbedtls_config.h (a mesma nos dois).
set -euo pipefail
DEPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${MBEDTLS_DIR:-$HOME/libpeer/third_party/mbedtls}"
CFG_DIR="$DEPS_DIR/mbedtls-xenon"
if [ "${HOST:-0}" = 1 ]; then
    OUT="${OUT:-/tmp/mbedtls-host}"
    CC=gcc; AR=ar
    FLAGS="-O2 -g"
else
    OUT="${OUT:-$HOME/mbedtls-xenon}"
    DKX="${DEVKITXENON:-$HOME/devkitxenon}"
    CC="$DKX/bin/xenon-gcc"; AR="$DKX/bin/xenon-ar"
    # Mesmo MACHDEP do devkitxenon/rules (ABI igual ao app/libxenon).
    FLAGS="-O2 -g -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -ffunction-sections -fdata-sections"
fi
grep -q "3.4.0" "$SRC/include/mbedtls/build_info.h" || { echo "esperava mbedTLS 3.4.0 em $SRC"; exit 1; }
mkdir -p "$OUT/obj"
rm -f "$OUT"/obj/*.o
# Só o que a config usa (o resto compilaria vazio, mas assim fica rápido).
MODS="aes asn1parse asn1write base64 bignum bignum_core bignum_mod bignum_mod_raw
      chacha20 chachapoly cipher cipher_wrap constant_time ctr_drbg ecdh ecdsa ecp
      ecp_curves entropy entropy_poll error gcm hash_info md oid pem pk pk_wrap
      pkparse platform platform_util poly1305 rsa rsa_alt_helpers sha1 sha256
      sha512 ssl_ciphersuites ssl_client ssl_debug_helpers_generated ssl_msg ssl_tls
      ssl_tls12_client x509 x509_crt"
for m in $MODS; do
    "$CC" $FLAGS -I"$CFG_DIR" -I"$SRC/include" -I"$SRC/library" \
        -DMBEDTLS_CONFIG_FILE='"xc360_mbedtls_config.h"' \
        -c "$SRC/library/$m.c" -o "$OUT/obj/$m.o"
done
rm -f "$OUT/libmbedtls-xc360.a"
"$AR" rcs "$OUT/libmbedtls-xc360.a" "$OUT"/obj/*.o
rm -rf "$OUT/include"; mkdir -p "$OUT/include"
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$OUT/include/"
cp "$CFG_DIR/xc360_mbedtls_config.h" "$OUT/include/"
ls -la "$OUT/libmbedtls-xc360.a"
