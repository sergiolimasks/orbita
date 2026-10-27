/*
 * orbita v0.21.0 — rede Wi-Fi salva (XC360/WIFI.TXT, ao lado do BETAKEY.TXT).
 * A senha NÃO é gravada: só a PMK (PBKDF2 da senha com o SSID), que é o que
 * o WPA2 usa. Formato (texto, uma chave por linha):
 *   XCWIFI1
 *   ssid=<SSID em hex>
 *   pmk=<64 hex> | open
 * Nada disto vai pro log.
 */
#ifndef XC360_XW_STORE_H
#define XC360_XW_STORE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XW_STORE_FILE "WIFI.TXT"

typedef struct {
    char    ssid[33];
    int     ssid_len;
    int     secure;
    uint8_t pmk[32];
} xw_saved_t;

/* 0 = ok */
int    xw_store_parse(const char *txt, size_t n, xw_saved_t *o);
/* devolve o tamanho (0 = não coube) */
size_t xw_store_format(char *out, size_t cap, const xw_saved_t *s);

#ifdef __cplusplus
}
#endif
#endif
