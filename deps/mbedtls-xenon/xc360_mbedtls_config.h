/*
 * XCloud360 v0.17 — configuração mínima do mbedTLS 3.4.0 (cliente HTTPS).
 * Usada IGUAL no Xbox (xenon-gcc, bare metal) e no teste do host (gcc).
 *
 * - TLS 1.2 cliente só (todos os hosts da Microsoft que usamos aceitam
 *   TLS 1.2 com ECDHE-RSA-AES-GCM; TLS 1.3 no 3.4 exige PSA — fora).
 * - Sem sockets (net_sockets.c), sem arquivo (FS_IO), sem timing.c.
 * - Hora: MBEDTLS_PLATFORM_TIME_ALT (o app chama mbedtls_platform_set_time)
 *   e gmtime_r próprio (MBEDTLS_PLATFORM_GMTIME_R_ALT).
 * - Entropia: MBEDTLS_NO_PLATFORM_ENTROPY + MBEDTLS_ENTROPY_HARDWARE_ALT
 *   (mbedtls_hardware_poll no app: jitter do timebase no Xbox).
 */
#ifndef XC360_MBEDTLS_CONFIG_H
#define XC360_MBEDTLS_CONFIG_H

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

#endif
