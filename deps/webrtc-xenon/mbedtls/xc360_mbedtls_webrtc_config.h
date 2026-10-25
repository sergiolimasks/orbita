/*
 * XCloud360 stage 4 — mbedTLS 3.4.0 config for the on-Xbox WebRTC stack.
 *
 * SUPERSET of deps/mbedtls-xenon/xc360_mbedtls_config.h (login, v0.17) plus
 * what DTLS-SRTP / libpeer need. It is a superset ON PURPOSE: the final ELF can
 * only contain ONE mbedTLS (same symbols, struct layouts depend on the config),
 * so at integration the app links this build (~/webrtc-xenon/xenon/lib/
 * libmbedtls-xcwebrtc.a) INSTEAD of ~/mbedtls-xenon/libmbedtls-xc360.a and the
 * login code keeps working unchanged (every login option is kept verbatim).
 * See app/h264play/WEBRTC-STAGE4.md "mbedTLS: one library".
 *
 * Added vs login:
 *   - DTLS 1.2 (client + server role: libpeer can be either), DTLS-SRTP
 *     (use_srtp extension, key export), anti-replay, HelloVerify (MBEDTLS_SSL_DTLS_HELLO_VERIFY), cookies.
 *   - MBEDTLS_TIMING_C with MBEDTLS_TIMING_ALT (timing_alt.h next to this
 *     file; the clock comes from the port layer, xcw_now_ms()).
 *   - X.509 certificate WRITING (libpeer self-signs a fresh cert per session),
 *     PK write, PEM write, GENPRIME (only used when RSA certs are selected).
 *   - MD5 (libpeer TURN long-term credentials).
 *   - AES-CTR mode (libsrtp's mbedTLS crypto backend: AES_CM_128 keystream).
 */
#ifndef XC360_MBEDTLS_WEBRTC_CONFIG_H
#define XC360_MBEDTLS_WEBRTC_CONFIG_H

/* ------------------------------------------------ login config, verbatim */

/* plataforma */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_DEPRECATED_REMOVED

/* bignum / chave pública */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* hash / cifra */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* RNG */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_RENEGOTIATION
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
/* Servidores mandam registros de até 16KB; envio é pequeno. */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#define MBEDTLS_ERROR_C

/* ---------------------------------------------- WebRTC (DTLS-SRTP) adds */

#define MBEDTLS_SSL_PROTO_DTLS
#define MBEDTLS_SSL_DTLS_ANTI_REPLAY
#define MBEDTLS_SSL_DTLS_HELLO_VERIFY
#define MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_COOKIE_C

#define MBEDTLS_TIMING_C
#define MBEDTLS_TIMING_ALT

#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_MD5_C
#define MBEDTLS_CIPHER_MODE_CTR

#endif
