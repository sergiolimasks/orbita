# Log dos testadores beta (v0.20.1)

Desde a v0.20.1 o log de diagnóstico e o STAT do jogo vão por HTTPS pra
nuvem, **só pra quem é testador beta**. O UDP :9003 pro PC (o último
resquício do modo PC) saiu. Código: `source/login/xc_logup.{c,h}`.

## Opt-in: chave no pendrive

- Arquivo `XC360/BETAKEY.TXT`: uma linha, ASCII imprimível de 8 a 64
  caracteres, sem espaço no meio. Espaços e quebras de linha nas pontas são
  ignorados; maiúsculas/minúsculas ficam como estão. Cada testador tem a sua.
- **Sem o arquivo (ou vazio/inválido): o app não envia nada.** Ele guarda só
  as últimas 16 KB de log em RAM (`XLU_MIN_BYTES`), pra uma chave digitada
  depois já ter contexto.
- Nenhuma chave vai na build (nada de header gerado; o ELF não contém chave).
- ~~`XC360/CONSOLE.ID`~~ (até a v0.20.2). **v0.20.3**: o ID é de HARDWARE
  (`xcb_hwid`: fusíveis + MAC, ver BETA-GATE.md), passado pelo main com
  `xlu_set_console_id`; sem ele nada é enviado. Vai no corpo (`console`) e no
  header `X-Console`.

## API pro menu (tela de configurações / teclado na tela: próxima versão)

```c
#include "login/xc_logup.h"
int xc_beta_active(void);                 /* 1 = chave presente e não recusada */
int xc_beta_state(void);                  /* XC_BETA_UNKNOWN / NONE / ACTIVE / INVALID */
int xc_console_id(char *out, size_t cap); /* "" até existir; 32 hex */
int xc_beta_save_key(const char *key);    /* 0 = aceita (grava no próximo passo do worker), -1 = formato */
```

- Todas podem ser chamadas de qualquer thread (só memória + lock).
- `XC_BETA_UNKNOWN`: o worker ainda não leu o pendrive (logo depois do boot
  / enquanto o login roda).
- `XC_BETA_INVALID`: a ponte respondeu 401/403. O envio para até o próximo
  boot (ou até uma chave nova por `xc_beta_save_key`). Hoje o menu mostra o
  aviso "Chave beta inválida..." sob o hero.
- `xc_beta_save_key` valida, e o worker grava `BETAKEY.TXT` (o pendrive só é
  tocado pela hw 3), religa o envio e cria o ID se faltar. O worker roda no
  menu, então a gravação acontece segundos depois, com o menu aberto.

## Envio

```
POST https://x360.pistatrack.com/v1/logs
Content-Type: application/json
X-Api-Key: <BETAKEY.TXT>
X-Console: <ID de hardware, 32 hex>          (v0.20.3)
{"console":"<o mesmo ID>","app":"v0.20.1","game":"<nome UTF-8 ou vazio>","lines":["...", ...]}
```

| resposta | o que o app faz |
|---|---|
| 2xx (204) | tira do buffer as linhas enviadas |
| 401 / 403 | `XC_BETA_INVALID`, para de enviar, buffer cai pro mínimo |
| 413 | lote pela metade (até 8 KB); com 8 KB ainda 413 = descarta esse lote |
| outros 4xx (400...) | descarta esse lote (não insiste) + backoff |
| 429 / 5xx / rede | guarda e tenta depois: backoff 30 s, dobrando até 10 min |

- **Quem envia**: o worker do catálogo/capas (hw 3), por
  `xcc_set_worker_extra(xlu_step, xlu_finish)`. Ele só roda no menu, com a hw
  3 livre (sem login, sem sessão, fora do jogo). **No jogo nada é enviado**:
  as linhas (STAT a cada 2 s, WebRTC, decoder) esperam a volta pro menu, sem
  HTTPS perto do decoder/WebRTC. Com um jogo pausado no menu o worker
  também fica parado (regra da hw 3), então o lote sai quando a sessão fecha.
- **Quando**: 1º lote logo que a chave é lida; depois a cada 60 s; logo
  depois de `xlu_flush_soon()` (fim de cada sessão, erro de sessão, falha do
  login, PLAY recusado), respeitando 4 s entre lotes (a ponte aceita 30/min);
  ou com o buffer em 3/4.
- **TLS persistente** (`https_conn`, keep-alive) enquanto o worker vive; o
  `xcc_bg_stop` antes de um PLAY derruba um envio em voo (vira retry).
- **Buffer**: 200 KB em RAM (`XLU_BUF_BYTES`); cheio = descarta as linhas mais
  antigas (um quarto por vez). Linha até 600 bytes.
- **Corpo**: até 256 KB sem compressão (a ponte aceita até 512 KB; não há
  zlib no ELF). JSON com escape; UTF-8 inválido vira `?`.

## O que nunca sai (filtro em `xlu_redact`, antes de guardar a linha)

- segredos registrados com `xlu_add_secret`: o código do login (o main
  registra quando a tela de login aparece) e a própria chave beta;
- sequências de 32+ caracteres com cara de token (letras + dígitos, fora de
  hexa puro): JWT, tokens XBL/GSSV/MSA viram `[token]`. UUIDs e hashes (só
  hexa) ficam, porque são ids de sessão úteis no diagnóstico;
- e-mails viram `[email]`.

O código do app já não loga token, gamertag nem e-mail; o filtro é a segunda
linha de defesa.

## Certificado

A ponte usa Let's Encrypt: leaf -> YR2 -> Root YR (cross-assinado por ISRG
Root X1). `source/login/ca_roots.c` (gerado por `tools/gen_ca_roots.py`) tem
agora 6 raízes: as 4 de antes (DigiCert G2/G3, Microsoft RSA/ECC 2017) +
**ISRG Root X1** + **ISRG Root YR** (`letsencrypt.org/certs/gen-y/root-yr.pem`,
SHA-256 E5:7B:7E:6F:...:A8:6F).

## Testes (host)

`app/h264play/hosttest/logup/run_logup.sh [unit|live|all]`

- **unit** (ASan+UBSan, sem rede; transporte e relógio falsos, pendrive =
  `$XC_STORE`): 45 checks. Cobre: sem chave nada sai e o buffer fica no
  mínimo; chave curta/com espaço = sem chave; chave aparada; 1º lote, header,
  JSON (escape, UTF-8 inválido, nome com acento), ID criado e estável; período
  de 60 s, flush e intervalo mínimo; 429/502/rede com backoff dobrando e
  entrega depois; 413 diminui o lote e drena; buffer cheio descarta o mais
  antigo; 401 -> `XC_BETA_INVALID` e para; `xc_beta_save_key` grava e religa
  com a chave nova; filtro (código de login, JWT, e-mail; UUID/bigId/STAT
  intactos; a chave não vaza).
- **live**: 1 lote real pra `x360.pistatrack.com` pelo nosso `https.c`, com
  hora do host e com `XC_TIME_UNKNOWN=1`: 204 nos dois, cadeia validada pelas
  raízes embutidas. Console de teste `000...0001`, app `hosttest`. A chave vem
  de `~/credentials/xc360-logs-key` pelo ambiente (nunca em arquivo do repo).
  Pra conferir/apagar no R2: `python3 ~/tools/xc360-r2.py ls logs/` /
  `get <key>` / `rm <key>`.

## v0.20.3

- `xlu_set_console_id(id)` (boot) e `xlu_use_key(chave)`: a chave validada pelo
  gate beta (BETA-GATE.md) liga o envio sem regravar o `BETAKEY.TXT`.
- 403 (chave derrubada por duplicidade) para o envio igual ao 401.
- Testes: `run_logup.sh unit` 52/52 (ID do main, sem ID nada sai, X-Console,
  use_key, 403).
