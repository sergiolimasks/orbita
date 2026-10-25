# Stage 3: o Xbox inicia a sessão xCloud sozinho (signaling)

Status 2026-10-25: **módulo pronto e testado no host, ainda NÃO integrado** no
`main.c`/`Makefile`. Nenhuma sessão real foi aberta e nenhum token real foi usado.
O teste ao vivo fica pro hardware (ver "O que só o teste ao vivo responde").

O WebRTC no Xbox é de outro agente. Daqui sai só a API de signaling que ele chama.
O munging de SDP é dele (lista completa abaixo, pra não se perder).

## Licença
`source/session/xc_session.c` e `xs_machine.c` são **porte próximo** de
`~/green-nx/src/core/session.cpp`, de `auth.cpp:fetch_passport_token` e do loop de
tentativas de `src/switch/stream/engine.cpp` (green-nx, **GPL-3.0**), mais o fluxo
de `tools/xcloud-capture/rtp/capture-rtp.cpp`. `hosttest/session/gen_golden.cpp`
copia a montagem dos JSONs do `session.cpp`. Distribuir o ELF com isto = GPL-3.0
(registrar em THIRD_PARTY.md na integração).

## Arquivos (todos novos)
| arquivo | o quê |
|---|---|
| `source/session/xc_session.h/.c` | camada HTTP: credenciais (refresh→XSTS→gssv, passport), `/active` `/play` `/state` `/v1/waittime` `/connect` `/sdp` `/ice` `/keepalive` `DELETE`, parsers puros (Teredo, ICE, errorDetails). Usa o `https.c`/`xbox_auth.c`/cJSON do login (não tem um segundo cliente HTTP). |
| `source/session/xs_machine.h/.c` | máquina de estados (`xsm_step`) + caixa thread-safe pro WebRTC (`xsm_rtc_*`) + linha `STATE` pro menu. Portável (host e Xbox). |
| `source/session/xs_task.h/.c` | só Xbox: roda a máquina numa hw thread (pilha 256KB, igual ao `login_task.c`). |
| `source/session/xs_port.h`, `xs_port_xenon.c` | `xs_lock()`/`xs_unlock()` (Xbox: `lock()` do libxenon + lwsync). |
| `hosttest/session/session_test.c` | 19 cenários, 114 checagens, contra um GSSV falso. |
| `hosttest/session/mock_port.c/.h` | `xc_port.h` + `https_request()` falsos, com relógio virtual (30 min de fila rodam em ms). |
| `hosttest/session/golden.h`, `gen_golden.cpp` | corpos/cabeçalhos gerados pelo `nlohmann::json` do próprio green-nx. O C do Xbox é comparado byte a byte. |
| `hosttest/session/run_session.sh` | `unit` / `xenon` / `golden` / `all`. |

## API

### Camada HTTP (`xc_session.h`): uma chamada = um pedido HTTPS bloqueante
```c
xs_creds_t c; xs_creds_init(&c, "xgpuweb");      /* ou "xgpuwebf2p" (jogo com anúncio) */
xs_creds_load(&c, &e);                           /* TOKEN.TXT -> c.refresh  (XS_ELOGIN se não tiver) */
xs_creds_login(&c, &e);                          /* refresh(rotaciona+salva) -> user.auth -> XSTS gssv -> /v2/login/user */
xs_session_t s; xs_init(&s, &c);
xs_active_list(&c, act, XS_MAX_ACTIVE, &n, &e);  xs_delete_path(&c, act[i].path, &e);  xs_attach(&s, path, title);
xs_start(&s, "FORZAHORIZON5", &e);               /* POST /v5/sessions/cloud/play */
xs_get_state(&s, &st, &e);                       /* st.st, st.raw, st.queue_pos, st.wait_s, st.err_code/err_msg */
xs_wait_time(&s, &sec, &e);                      /* GET /v1/waittime/<titleId> (ETA da fila) */
xs_connect(&s, &e);                              /* passport + POST /connect (no ReadyToConnect) */
xs_sdp_offer(&s, offer, &e);  xs_sdp_poll(&s, &answer, &e);   /* XS_PENDING = 204 */
xs_ice_send(&s, cands, n, ufrag, &e);  xs_ice_poll(&s, &cands, &e);
xs_keepalive(&s, &e);  xs_stop(&s);              /* DELETE <sessionPath> */
```
Retornos: `XS_OK`, `XS_PENDING`, `XS_ERR`, `XS_EAUTH` (401 mesmo depois de
renovar), `XS_ELOGIN` (sem token/invalid_grant), `XS_ENET`, `XS_ETIMEOUT`,
`XS_ECANCEL`, `XS_ENOTFOUND` (404), `XS_EFAILED` (errorDetails real).
**401 em qualquer pedido ao GSSV:** renova o login uma vez (`xs_creds_login` com o
refresh em RAM, no máximo 3 vezes por credencial) e repete o pedido.

### Máquina de estados (`xs_machine.h`)
```c
xsm_cfg_t cfg; xsm_cfg_defaults(&cfg, title_id, name);   /* cfg.f2p, cfg.active_policy, tempos */
xsm_t m; xsm_init(&m, &cfg, NULL /* refresh em RAM ou NULL = TOKEN.TXT */);
while ((d = xsm_step(&m)) >= 0) dorme(d);               /* thread da sessão */
xsm_request_stop(&m);  xsm_answer_active(&m, 1/0);      /* menu */
xsm_state_line(&m, buf, cap);                           /* "STATE\tQUEUE\tNa fila: posicao 3, ~2 min" */
```
Fases: `AUTH → ACTIVE (→ ASK) → START → POLL → WAIT_OFFER → SDP → ICE_BRIDGE →
CONNECTING → STREAM_WAIT → STREAMING`, mais `COOLDOWN`, `STOPPING`, `DONE` e `ERROR`.

Política de sessão já ativa (`cfg.active_policy`): `XSM_ACTIVE_CANCEL` (padrão,
igual ao `cleanup_stale_sessions` do capture-rtp: DELETE em todas), `XSM_ACTIVE_RESUME`
(mesma titleId: retoma sem /play; outras: DELETE), `XSM_ACTIVE_ASK` (para em `ASK`
mostrando "Sessao ja ativa: A retoma, B cancela" até `xsm_answer_active`).

### Linhas STATE pro menu (mesmos códigos e textos do launcher do PC, ASCII)
| quando | linha |
|---|---|
| login | `STARTING` "Autenticando na Microsoft..." |
| /active | `STARTING` "Verificando sessoes ativas..." / "Encerrando sessao antiga..." / "Retomando a sessao..." |
| /play | `STARTING` "Iniciando <nome>..." |
| New/Provisioning/ReadyToConnect | `STARTING` "Preparando servidor..." |
| WaitingForResources | `QUEUE` "Na fila: posicao N, ~M min" / "Na fila: ~M min" / "Na fila do xCloud..." (= `queue_text()` do launcher) |
| Provisioned, SDP/ICE | `STARTING` "Conectando video..." |
| DTLS ok | `STARTING` "Aguardando video..." |
| 1º vídeo | `STREAMING` "<nome>" |
| re-sinalização | `RECONNECTING` "Reconectando: ..." |
| nova tentativa | `STARTING` "Tentando de novo..." |
| fim | `IDLE` "Sessao encerrada" |
| erros | `ERROR` "Sessao xCloud falhou <code>", "Falha ao iniciar jogo no xCloud: HTTP 4xx em play (code)", "Falha na negociacao SDP: ...", "xCloud nao conectou (midia morta)", "Tempo esgotado esperando o xCloud (<estado>)", "Sem resposta do xCloud: ...", "Login expirou: entre de novo no Xbox", "Login falhou: ...", "Faca o login no Xbox primeiro", "A conexao caiu varias vezes, comece de novo" |

### Contrato com o WebRTC (thread de rede, nunca bloqueia)
A thread de rede **não pode fazer HTTPS**: o `net_pipe` é servido por ela mesma e
travaria. Ela conversa com a sessão só pela caixa:
1. `gen = xsm_rtc_gen(m)`. Se a `gen` mudou, derrubar o PC velho (é tentativa nova
   ou re-sinalização).
2. `xsm_rtc_want_offer(m, &gen)`: quando der 1, criar o PC, fazer o offer, aplicar o
   **munging** e chamar `xsm_rtc_set_offer(m, gen, sdp)`. Cada candidato local do
   `onicecandidate` vai pra `xsm_rtc_add_local(m, gen, cand)`: string crua do libpeer,
   como no capture-rtp, até 16 candidatos.
3. `xsm_rtc_take_answer(m, gen)` dá o answer (malloc) só **depois** do "pre-SRD
   bridge": locais já mandados e ≥1 remoto em mãos (ou 8 s). Aí é
   `set_remote_description(answer)` e, em seguida, `xsm_rtc_take_remote(...)`, com
   `add_ice_candidate` para cada remoto. Continuar chamando `take_remote` (chegam mais).
4. Informar: `XSM_RTC_ICE_UP` (CONNECTED), `XSM_RTC_MEDIA_UP` (COMPLETED = DTLS/SCTP
   ok), `XSM_RTC_VIDEO` (1º AU decodificável), `XSM_RTC_DEAD` (FAILED/CLOSED antes de
   subir) e `XSM_RTC_DROPPED` (caiu no meio do jogo).
   Morte de mídia: ICE ok e sem MEDIA_UP em 12 s, ou nada em 17 s depois do answer.
   Nesses casos a sessão faz DELETE, espera 3 s e abre outra (2 tentativas), como o
   capture-rtp. DROPPED re-sinaliza a MESMA sessão (`/state` → `/connect` → offer
   novo, até 3 vezes em 2 min, igual ao engine.cpp #61). Se isso não der certo, abre
   sessão nova.

## Endpoints, tokens e cabeçalhos
Cabeçalhos de TODO pedido ao GSSV (`<região>` = `baseUri` default do
`/v2/login/user`, ex. `brs.core.gssv-play-prod.xboxlive.com`). A ordem e o texto são
os de `GssvSession::headers()`, conferidos byte a byte:
```
Accept: application/json
Content-Type: application/json
X-Gssv-Client: XboxComBrowser
X-MS-Device-Info: {"appInfo":{"env":{"clientAppId":"www.xbox.com","clientAppType":"browser","clientAppVersion":"26.1.97","clientSdkVersion":"10.3.7","httpEnvironment":"prod","sdkInstallId":""}},"dev":{"browser":{"browserName":"chrome","browserVersion":"140.0.3485.54"},"displayInfo":{"dimensions":{"heightInPixels":720,"widthInPixels":1280},"pixelDensity":{"dpiX":1,"dpiY":1}},"hw":{"make":"Microsoft","model":"unknown","sdktype":"web"},"os":{"name":"android","platform":"desktop","ver":"22631.2715"}}}
Authorization: Bearer <gsToken>
```
O device-info é o do tier P720 (`osName android`, 1280x720), que é o que o
capture-rtp usa. Ele está com as chaves em ordem alfabética, porque é assim que o
`nlohmann::json` do green-nx serializa. O `DEVICE_INFO` do `xbox_auth.c` (catálogo)
tem outra ordem e funciona, mas a sessão usa a do PC.

| passo | pedido | token | corpo / resposta lida |
|---|---|---|---|
| refresh | POST login.microsoftonline.com/consumers/oauth2/v2.0/token | refresh MSA (TOKEN.TXT) | `xa_refresh` do login; o refresh rotacionado é salvo no TOKEN.TXT |
| user/XSTS | user.auth.xboxlive.com, xsts.auth.xboxlive.com (RP `http://gssv.xboxlive.com/`) | access → user token | `xa_user_authenticate`/`xa_xsts_authorize` |
| gssv login | POST `<offering>`.gssv-play-prod.xboxlive.com/v2/login/user | XSTS | `gsToken` + `offeringSettings.regions[isDefault].baseUri` |
| sessão ativa | GET `/v5/sessions/cloud/active` | gsToken | qualquer `sessionPath`/`path` (+`titleId`/`state` do mesmo objeto); 404/204 = nenhuma |
| play | POST `/v5/sessions/cloud/play` | gsToken | `{"clientSessionId":"","fallbackRegionNames":[],"serverId":"","settings":{"enableOptionalDataCollection":false,"enableTextToSpeech":false,"highContrast":0,"locale":"en-US","nanoVersion":"V3;WebrtcTransport.dll","osName":"android","sdkType":"web","timezoneOffsetMinutes":120,"useIceConnection":false},"systemUpdateGroup":"","titleId":"<T>"}` → `sessionPath` |
| estado | GET `<sessionPath>/state` a cada 1 s | gsToken | `state` (New/Provisioning/WaitingForResources/ReadyToConnect/Provisioned/Failed/Error), `errorDetails.code/message`, fila em qualquer chave `*position*`/`placeInQueue` e `estimatedTotalWaitTimeInSeconds` |
| ETA da fila | GET `/v1/waittime/<titleId>` (a cada 30 s, só na fila, e só se o /state não trouxe tempo) | gsToken | `estimatedTotalWaitTimeInSeconds` (ou alloc+prov). **Não** é usado pelo PC (vem do greenlight/xbox.com); `cfg.waittime=0` desliga |
| passport | POST login.live.com/oauth20_token.srf (form) | **refresh MSA** | `client_id=1f907974-...&scope=service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN&grant_type=refresh_token&refresh_token=<rt>` → `access_token`. Um `refresh_token` que venha junto é ignorado, como no green-nx |
| connect | POST `<sessionPath>/connect` uma vez, no 1º ReadyToConnect | gsToken (header) + passport (corpo) | `{"userToken":"<passport>"}`. Se não vier 2xx, busca outro passport e tenta mais uma vez |
| SDP | POST `<sessionPath>/sdp`, depois GET a cada 500 ms (até 60 s, 204 = ainda não) | gsToken | `{"configuration":{chat 1-1, chatConfiguration opus/webm 24 kHz mono 20 ms, control 1-3, input 1-9, message 1-1, reliableinput 9-9, unreliableinput 9-9},"messageType":"offer","requestId":"1","sdp":"..."}`. A resposta é `exchangeResponse` (string JSON) com `.sdp`. `errorDetails` com tudo null = sucesso |
| ICE | POST `<sessionPath>/ice`, depois GET a cada 200 ms até `end-of-candidates` | gsToken | `{"candidate":["{\"candidate\":\"<c>\",\"sdpMLineIndex\":0,\"sdpMid\":\"0\",\"usernameFragment\":\"<ufrag do offer>\"}", ..., "{...\"a=end-of-candidates\"...}"],"messageType":"iceCandidate"}`. Remotos: `exchangeResponse` = array (ou `{candidates:[]}`) de objetos ou strings. Tira o `a=`, descarta IPv6 que não é Teredo, e cada Teredo vira `candidate:<20+> 1 UDP 1 <ipv4> <porta>` e `... 9002 typ host` |
| keepalive | POST `<sessionPath>/keepalive` (corpo vazio) a cada 5 s, começando depois do answer | gsToken | 404 = sessão morreu (nova tentativa) |
| fim | DELETE `<sessionPath>` | gsToken | erros ignorados |

## Munging de SDP e o que mais o capture-rtp faz no lado WebRTC (NÃO está aqui)
Em ordem, no `capture-rtp.cpp` (`main()`, por volta das linhas 1300-1510):
1. PC do libpeer com `video_codec=H264`, `audio_codec=OPUS`, `datachannel=BINARY` e
   `ice_servers[0]="stun:stun.l.google.com:19302"`. Um PC **novo por tentativa** (o
   estado dtls/sctp do libpeer fica preso na criação).
2. Os handlers de datachannel são registrados ANTES do offer. Os canais
   control/input/message/chat (SIDs 0/2/4/6) só abrem no `on_channel_open`, com o
   SCTP de pé.
3. `peer_connection_create_offer` vem 300 ms depois de o loop do PC começar.
4. **`a=setup:passive` → `a=setup:actpass`**, em TODAS as ocorrências.
5. **Teto de banda do vídeo:** logo depois da linha `c=` da seção `m=video`, inserir
   `b=AS:<kbps>` e `b=TIAS:<kbps*1000>`, respeitando o `\r\n` ou `\n` que o SDP já usa.
   Isso só entra se houver `m=video` com `c=` antes do próximo `m=`. O kbps padrão é
   3000 no capture-rtp, mas **o launcher passa `--kbps 1500`**. O REMB sozinho não é
   respeitado.
6. O `a=ice-ufrag:` do offer vira o `usernameFragment` dos candidatos locais. A
   sessão já tira esse valor do offer e não precisa de mais nada.
7. **Não** usados pelo capture-rtp (existem no green-nx, mas só valem para os tiers
   1080p): `sdp_force_stereo` (`useinbandfec=1` → `;stereo=1`) e
   `sdp_scale_video_caps_1080` (`max-fs=3600`→`8160`, `max-mbps=108000`→`489600`).
8. O answer entra **sem mudança** no `set_remote_description(SDP_TYPE_ANSWER)`. A
   cópia em disco é só redigida (`a=ice-pwd`, Bearer...).
9. Pre-SRD: primeiro manda os locais (ou, se ainda não houver nenhum, as linhas
   `a=candidate:` do offer sem o `a=`), depois espera ≥1 remoto (40x200 ms) e só então
   faz o SRD. A caixa da sessão entrega o answer já nessa ordem.
10. Depois do SRD: ICE contínuo (200 ms) e keepalive a cada 5 s. COMPLETED (DTLS) em
   até 12 s depois de o ICE conectar; se não vier, é mídia morta e a tentativa recomeça.
11. Depois do DTLS: handshake no canal `message`, autorização + gamepad + resolução
   + capacidades (`xcloud_protocol.cpp`: `startup_messages(w,h,kbps,fps)`, fps 30).
   Também RTCP RR + **REMB a cada 1 s** com o kbps (sem REMB o xCloud ignora o
   maxBitrate), PLI repetido 1x/s enquanto o cliente espera IDR, e o libpeer com os
   patches do green-nx (`peer_connection_loop` retorna >0, `send_remb`,
   `lookup_sid`, `datachannel_send_text_sid`).

## Threads
- hw 0 (main/menu): lê `xst_state_line()` e passa pro `mm_handle_line()`.
- hw 1 (rede): lwIP, `np_step`, e o WebRTC do outro agente. Só usa `xsm_rtc_*`.
- hw 3 (sessão, `XST_HW_THREAD`): `xsm_step` + HTTPS pelo pipe. **Nunca ao mesmo
  tempo que o login_task**: os dois usam a hw 3, e o `xcn_cancelled()` do `https.c` é
  definido no `login_task.c`.
- **Conflito:** a sessão precisa continuar viva DURANTE o jogo (keepalive, ICE,
  re-sinalização), mas hoje o decoder ocupa as hw 4,5,2,3 no PLAY. Escolher uma
  destas saídas: (a) o decoder com 3 threads (`DECODE_THREADS=3`, deixando a hw 3
  livre); (b) mudar `XST_HW_THREAD` para uma thread que o decoder não use; (c)
  depois do STREAMING, reduzir a sessão a keepalive e rodar o `xsm_step` em outra
  thread de baixa prioridade. Não dá para rodar no main: cada passo bloqueia até
  20 s (timeout de IO do https.c).
- Cancelar não interrompe um pedido em voo: o `xsm_request_stop` vale no próximo
  passo, e cada pedido demora no máximo uns 20 s.

## Integração (passos exatos, para quem for mexer no main.c/Makefile)
1. **Makefile:** dentro do bloco `ifeq ($(XBOX_LOGIN),1)`, junto do `SOURCES +=
   source/login ...`, adicionar uma opção:
   ```make
   XBOX_SESSION ?= 0
   ifeq ($(XBOX_SESSION),1)
   SOURCES      += source/session
   LOGIN_CFLAGS += -DXCLOUD360_SESSION=1
   endif
   ```
   (`source/session/*.c` compila inteiro: `xs_task.c`/`xs_port_xenon.c` são
   `#ifdef XENON`, e o hosttest não fica em `source/`.) Trocar a opção exige `make clean`.
   Se preferir lista de objetos: `xc_session.o xs_machine.o xs_task.o xs_port_xenon.o`.
2. **main.c**, `#ifdef XCLOUD360_SESSION`, `#include "session/xs_task.h"`.
3. **PLAY** (no `if (PRESSED(a))` do menu): com o catálogo do Xbox (`g_cat_src == 1`)
   e `!lt_running() && !xst_running()`, chamar
   `xst_start(g->id, g->name, (flags & XA_F_ADPLAY) != 0, XSM_ACTIVE_CANCEL, NULL)`
   no lugar de `ctrl_send_line("PLAY\t...")`. Fazer o mesmo `waiting = 1`,
   `g_mm.state = MM_ST_NONE` etc. Para isso, o `mm_game_t`/`xa_title_t` tem que
   carregar o `XA_F_ADPLAY`: hoje o `xbox_catalog_commit` manda só id/nome. Uma saída
   é guardar as flags num array paralelo.
4. **Loop do menu:** `static unsigned seen; char l[200]; unsigned s = xst_state_line(l, sizeof l);`
   `if (s != seen) { seen = s; mm_handle_line(&g_mm, l); }`. Assim a tela de espera
   mostra STARTING/QUEUE/ERROR do Xbox exatamente como mostrava os do PC.
5. **B na espera / Back+Start no jogo / sair:** `xst_stop()` no lugar do (ou além do)
   `send_stop()`. A thread faz o DELETE e sai sozinha.
6. **Login:** não chamar `lt_start()` (Y no menu) com `xst_running()`, e vice-versa.
   O refresh vem do TOKEN.TXT; o login_flow já o deixou lá, e a sessão grava a
   rotação. O `xc_usb_hold` do main tem que continuar atendendo enquanto a sessão
   roda: ela grava no pendrive no AUTH e em cada re-login por 401.
7. **WebRTC:** o outro agente chama `xsm_rtc_*` em `xst_machine()` do `np_step`/loop
   dele, seguindo o "Contrato com o WebRTC" acima.
8. **Threads do decoder:** resolver o conflito da hw 3 (seção Threads) antes de ligar
   `XBOX_SESSION=1` com vídeo.
9. **Teste ao vivo só do signaling (sem WebRTC):** `xsm_cfg_defaults` +
   `cfg.max_attempts = 1` + `xst_start_cfg(&cfg, NULL)`. A máquina vai até
   `Provisioned` (testa login, /active, /play, fila, /connect). Como o offer nunca
   vem, ela cai em "WebRTC nao gerou a oferta" depois de 30 s e faz DELETE. Com
   `max_attempts` 2, ela abriria uma 2ª sessão.
10. THIRD_PARTY.md: "source/session: porte do green-nx (GPL-3.0)".

## Testes
`app/h264play/hosttest/session/run_session.sh` (padrão `all` = `unit` + `xenon`):
- **unit:** gcc com ASan/UBSan e `-Wall -Wextra -Werror`, 19 cenários e **114 checagens,
  0 falhas**:
  - **puro:** play/connect/sdp/ice/headers iguais byte a byte ao green-nx (`golden.h`);
    Teredo (1.2.3.4:3074, forma curta e comprimida); ICE em array ou em
    `{candidates}`; strings JSON aninhadas; IPv6 não-Teredo descartado; errorDetails
    só com null = ok; answer com erro; fila; walker do `/active`; waittime; ufrag.
  - **caminho feliz:** auth → active 404 → play → Provisioning → ReadyToConnect
    (passport + connect) → Provisioned → offer → 2×204 → answer → POST /ice (locais
    + end) → remotos (Teredo decodificado) → answer publicado → MEDIA_UP → VIDEO →
    STREAMING → keepalives → stop → DELETE. Confere a ordem dos pedidos, as linhas
    STATE, o refresh rotacionado salvo e **nenhum token no log**.
  - **fila:** WaitingForResources sem info + `/v1/waittime` → "Na fila: ~10 min";
    `/state` com posição → "Na fila: posicao 3, <1 min"; waittime pedido 1 vez em 4 s.
  - **Failed:** 2 tentativas com 3 s de intervalo, 2 DELETEs, e
    "ERROR Sessao xCloud falhou SessionProvisioningFailed".
  - **401 → re-auth:** o gsToken expira no 2º /state, vem refresh+XSTS+login de novo
    (GS-2) e tudo depois sai com o token novo; refresh rotacionado 2x. 401
    persistente → "Login falhou: HTTP 401 em ...". `invalid_grant` → "Login expirou".
    Sem TOKEN.TXT → zero pedidos.
  - **timeouts:** Provisioning sem fim (2×210 s + 3 s → ERROR); a fila não corta
    antes de 30 min; SDP sem answer (60 s, um PC por tentativa); timeout de rede no
    /state (5x → ERROR); mídia morta (ICE ok, DTLS mudo 12 s → sessão nova → STREAMING).
  - **sessão ativa:** cancelar (DELETE antes do play); retomar (sem /play, poll no
    path antigo); outro jogo com política retomar → cancela; perguntar → cancelar;
    perguntar e o usuário sai → nada é deletado.
  - **queda no meio do jogo:** re-sinaliza a mesma sessão (1 play, 2 connect, 2 sdp, 2
    PCs, linha RECONNECTING, volta pro STREAMING).
- **xenon:** `xenon-gcc` com `MACHDEP` + `-g -O2 -Wall` + as defines do login, nos
  4 .c de `source/session`: **0 warnings, 0 erros**. Também passa com
  `XENON_EXTRA="-Wextra -Wshadow -Wformat=2"`. Tamanho: xc_session.o 22.6 KB de text,
  xs_machine.o 15.4 KB, xs_task.o 1.4 KB + 272 KB de bss (pilha 256 KB + xsm_t).
- As respostas falsas seguem formatos reais: `/state` vem dos `[XCLOUD360-STATE]` dos
  logs do launcher (`{"errorDetails":null,"serverPlatform":"XBOX","state":...}`); a
  região `brs.core` vem do `session-meta.json`; o resto vem dos campos que o green-nx
  lê. Ids e tokens são inventados (`GS-1`, `FAKE-REFRESH-0`, `PASSPORT-FAKE`).

## O que só o teste ao vivo responde
1. **User-Agent / Accept-Encoding:** o `https.c` manda `User-Agent: XCloud360/0.17` e
   `Accept-Encoding: identity`. O libcurl do green-nx não manda UA nenhum e anuncia
   gzip/br. O catálogo funciona assim, mas não sei se /play e /sdp funcionam. Se o
   GSSV recusar, a correção é uma linha no `https.c` (UA opcional), que hoje está
   fora do meu escopo.
2. **Custo do TLS:** cada pedido é uma conexão nova (`Connection: close`), com
   handshake RSA/ECDHE no Xenon. O PC reaproveita a conexão do curl. A 200 ms de poll
   de ICE e 5 s de keepalive, isso pode ser pesado. Se for, subir
   `cfg.ice_poll_ms`/`keepalive_ms` (o green-nx usa 15 s) ou pôr keep-alive no https.c.
3. Formato do `/v5/sessions/cloud/active` (o walker aceita qualquer
   `sessionPath`/`path`), e se uma sessão retomada em `Provisioned` aceita um
   `/sdp` novo (o engine.cpp diz que sim para a re-sinalização). Também: o
   `/v1/waittime` responde nessa região? E o `/state` da fila traz posição em
   algum momento? Nos logs gravados, nunca trouxe.
4. **Corpo de erro** do `/play` (conta sem Game Pass, jogo indisponível, limite de
   sessões): o texto mostrado sai do `code`/`error`/`errorDetails.code`.
5. **Passport** do login.live.com com o refresh token **rotacionado** pelo
   microsoftonline. O green-nx faz igual, mas a cadeia de certificado do
   login.live.com no Xbox não foi testada. Medido do devbox: DigiCert Global G2 TLS
   RSA SHA256 2020 CA1 → **DigiCert Global Root G2**, que está nas raízes embutidas.
6. A **USB** gravando o TOKEN.TXT a partir da hw 3 durante o jogo (re-login por 401).
7. Se o keepalive é mesmo necessário a cada 5 s, ou se 15 s bastam (green-nx).
8. Timezone/locale: são iguais ao PC (`en-US`, `timezoneOffsetMinutes:120`). Trocar
   para pt-BR é uma linha (`s->locale`), mas deixei como no PC.
