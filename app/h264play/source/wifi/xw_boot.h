/*
 * orbita v0.21.5 — decisão de rede no boot (C puro, testada no host em
 * hosttest/wifi). O main chama a cada volta do menu até dar XWB_GO; enquanto
 * não dá, a chave beta, a atualização, o login e as capas esperam.
 *
 *   cabo com DHCP .............................. GO (como sempre; o Wi-Fi nem liga)
 *   Wi-Fi com IP e rota padrão ................. GO
 *   usuário pulou (B: continuar sem internet) .. GO
 *   adaptador ainda não visto (até ~8 s) ....... FINDING  "Procurando o Wi-Fi interno"
 *   sem adaptador (Fat) ou adaptador com falha . NOINTERNET "Sem internet: conecte o cabo"
 *   rede salva, tentando (até 30 s) ............ CONNECTING "Conectando ao Wi-Fi "SSID"" + etapa
 *   sem rede salva, ou a salva falhou .......... PICK  tela do Wi-Fi aberta direto
 */
#ifndef XC360_XW_BOOT_H
#define XC360_XW_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

#define XWB_AUTO_MS 30000u   /* tempo máximo tentando a rede salva antes de abrir a lista */

typedef struct {
    int      cable;          /* o cabo tem DHCP (no boot ou depois, no "Tentar de novo") */
    int      adapter;        /* -1 ainda procurando, 0 não tem, 1 tem */
    int      adapter_failed; /* o adaptador existe mas não ligou */
    int      saved;          /* há rede salva (WIFI.TXT) */
    int      wifi_ok;        /* Wi-Fi com DHCP e rota padrão */
    int      auto_gave_up;   /* o driver desistiu da rede salva (senha/segurança) */
    unsigned auto_ms;        /* tempo tentando a rede salva (desde o adaptador ligar) */
    int      skipped;        /* o usuário escolheu seguir sem internet */
} xwb_in_t;

enum { XWB_GO = 0, XWB_FINDING, XWB_CONNECTING, XWB_PICK, XWB_NOINTERNET };

int         xwb_decide(const xwb_in_t *in);
const char *xwb_name(int d);

#ifdef __cplusplus
}
#endif
#endif
