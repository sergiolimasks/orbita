# Wi-Fi interno do Xbox 360 S/E (v0.21.0)

Status (2026-10-26): v0.21.0 rodou num Slim (Corona) e não ligou (ver "v0.21.1" no fim); v0.21.1 = high speed no EHCI (funcionou: firmware, scan); v0.21.4 = log do WPA2, erros com prova, corrida EP1/EP2; v0.21.5 = app só com Wi-Fi (boot sem cabo, WIFILOG.TXT); v0.21.6 = RxPD tipo 2 (WPA2 conecta); v0.21.7 = pendrive e Wi-Fi juntos, vigia (HANG.TXT), catálogo do pendrive.
O primeiro teste no console é o do usuário; o log enviado (`[wifi] ...`) foi
pensado pra diagnosticar cada etapa (tabela no fim).

O app conecta no Wi-Fi interno do 360 S/E (Marvell 88W8786U, USB `045e:0765`):
tela de Wi-Fi nas Configurações, lista de redes, senha no teclado da tela,
conexão aberta ou WPA2-PSK, e o lwIP do app passa a sair pelo Wi-Fi.

## Regras

- **Ethernet igual a antes.** O adaptador só é ligado se (a) existe rede salva
  (`XC360/WIFI.TXT`) **e** o cabo não pegou DHCP no boot, ou (b) o usuário abriu
  a tela do Wi-Fi e pediu algo (procurar / conectar). Sem isso o código só olha
  se o dispositivo existe (leitura, a cada ~0,5 s, desiste em 30 s sem achar).
- O Wi-Fi só vira a **rota padrão** do lwIP quando o cabo não pegou DHCP no
  boot (`stream_net_cable()`). Com cabo, uma conexão manual sobe mas o app segue
  no cabo (a tela diz isso).
- **Xbox Fat / sem adaptador:** a tela diz "Wi-Fi não encontrado neste console
  (só Xbox 360 S/E com Wi-Fi interno)".
- **Só aberta e WPA2-PSK** (par CCMP, grupo CCMP ou TKIP). WPA3/SAE, PMF
  obrigatório, PSK-SHA256, 802.1X (corporativa), WEP e WPA1 aparecem na lista
  como "Não suportada". Rede de transição WPA2/WPA3 conecta como WPA2.
- **Senha nunca é gravada nem logada.** O main faz o PBKDF2 (senha + SSID ->
  PMK) e apaga a senha na hora; só a PMK vai pra thread de rede e pro pendrive.
  O log tem SSID, canal, dBm, os 3 primeiros bytes do BSSID, IP da LAN; nunca
  senha, PMK, PTK, GTK nem MAC completo.

## Arquivos

| arquivo | o que é |
|---|---|
| `source/wifi/mwx_proto.{h,c}` | protocolo do firmware (C puro): moldura USB, comandos, scan/assoc/chaves, TxPD/RxPD, classificação de segurança |
| `source/wifi/xw_wpa.{h,c}` | WPA2 no host: PBKDF2, PRF-SHA1, PTK, MIC, AES key wrap/unwrap, 4-way + rekey de grupo (mbedTLS: HMAC-SHA1 e AES) |
| `source/wifi/xw_core.{h,c}` | driver (máquina de estados, C puro, ops injetadas): probe/boot, init, fila de comandos, scan, conexão, chaves, reconexão |
| `source/wifi/xw_store.{h,c}` | `WIFI.TXT` (SSID em hex + PMK, ou `open`) |
| `source/wifi/xw_xenon.{h,c}` | Xbox: USB pela pilha OHCI/CFE do libxenon, netif `wl` do lwIP + DHCP, mailbox com o menu, boot |
| `source/wifi/xw_ehci_xenon.c` | tira o adaptador do tinyehci e entrega a porta ao OHCI |
| `source/beta/xc_keypad.{h,c}` | + `xtk_*`: teclado de texto (senha), mesmo modelo do teclado da chave |
| `source/ui/xui_home.c`, `xui.h` | `xui_wifi_frame`, `xui_textkey_frame`, 2º botão nas Configurações |
| `source/main.c` | telas `SCR_WIFI`/`SCR_WKEY`/`SCR_NETWAIT`, `xw_sys_step` no `net_hook`, espera do boot |
| `source/login/net_pipe.c` | `np_dns_reload`: DNS do DHCP do Wi-Fi |
| `source/tcp_stream.c` | `stream_net_cable`: o cabo pegou DHCP no boot? |
| `hosttest/wifi/` | `run.sh` + `wifi_unit.c` (408 verificações + conferência com Python) |

Referência: patch do Free60 (`Free60Project/linux-xbox360-wifi`, boot "MRVL")
e o `mwifiex` do Linux (GPL-2.0), linha em `THIRD_PARTY.md`.

## USB: por que full speed no OHCI

O `EHCI_Init` do libxenon (tinyehci) fica com toda porta high-speed e só fala
mass storage, síncrono (um pedido por vez, espera ocupada, timeout global; um
bulk IN pendente bloquearia a hw1 e perderia dados no timeout). O adaptador é
USB 2.0, então cai lá. `xw_ehci_release_wifi` marca o dispositivo `busy` (o
`USBStorage_Init` de cada `usb_do_poll` pula a porta), zera o `id` e liga
`PORT_OWNER`: a porta vai pro OHCI companheiro, o adaptador reenumera em **full
speed** e a pilha CFE (assíncrona, a mesma dos controles e já com as correções
do GIP: descritores OHCI não-cacheados) o vê **sem driver**. O app abre os
canos e pendura os pedidos:

- EP1 OUT/IN (comando/evento; IN interrupt usa `usb_open_pipe_interval`), 1 IN de 2 KB;
- EP2 OUT (dados, até 4 em voo de 8 buffers de 2 KB, ZLP quando o tamanho é múltiplo do MPS) e 3 IN de 4 KB;
- buffers `memalign(128)` (linha de cache do Xenon), callbacks só marcam; tudo é
  processado no `xw_sys_step`.

Banda: full speed = 12 Mbit/s no barramento, uns 6–8 Mbit/s úteis com o
polling de 1 ms. O stream pede 1,5 Mbit/s (`NATIVE_KBPS`).

## Firmware (88W8786U)

1. **Probe** (patch do Free60): `FUNC_INIT` com moldura. A ROM responde 8 bytes
   (`01 00 00 00 ...`), o firmware rodando responde `0xF00DFACE`.
2. **ROM:** manda `"MRVL" 01 00...` (16 bytes); a ROM sobe o firmware da flash e
   manda o evento `INIT_DONE` (até 10 s). Nada de arquivo de firmware.
3. **Init:** `FUNC_INIT`, `GET_HW_SPEC` (MAC, versão, cap, TLV da API de chave),
   `MAC_CONTROL` (RX|TX|Ethernet II). Sem power save, sem 11n (associação b/g:
   54 Mbit/s no ar, sobra pro USB full speed; evita BA/AMPDU/AMSDU).
4. **Scan** (legado, `ext_scan` não existe nesse firmware): 6 comandos (canais
   1 / 2-5 / 6 / 7-10 / 11 / 12-13, ativo 40 ms). Conexão: scan específico do
   SSID nos 13 canais (pega rede oculta salva).
5. **Associação:** SSID, DS (canal), CF, taxas em comum, auth open, IE RSN
   (`30 14 01 00 | grupo | CCMP | PSK | 00 00`).
6. **Chaves:** `KEY_MATERIAL` v1 (TLV 0x0100) ou v2 (TLV 0x019c) conforme o TLV
   `API_REV` do `GET_HW_SPEC`. A PTK entra 30 ms depois da msg 4 (pra msg 4 sair
   sem cifra); GTK TKIP com as chaves de MIC trocadas, como o wpa_supplicant.
7. **Dados:** `[TxPD 20 B][Ethernet II]`; RX `[RxPD][802.3 + SNAP]` vira Ethernet II.
   Eventos `LINK_LOST`/`DEAUTH`/`DISASSOC` derrubam o link e reconectam (3 s, 6, 12... até 30 s).

### WPA2: supplicant embutido x handshake no host

**Handshake no host.** O `mwifiex` do Linux não tem caminho de supplicant
embutido pra STA: o bit 21 do `fw_cap_info` ("firmware supplicant") só é usado
pra rekey offload em suspensão (`GTK_REKEY_OFFLOAD_CFG`), e passphrase no
firmware só existe no modo AP (`TLV_TYPE_UAP_WPA_PASSPHRASE`). O patch do Free60
conecta com `wpa_supplicant -D nl80211`, ou seja, EAPOL no host + `KEY_MATERIAL`:
esse é o caminho que já funcionou nesse firmware. O log mostra o bit 21 mesmo
assim (`supplicant no firmware: sim/não, não usado`).

`xw_wpa.c`: msg 1 -> SNonce (CTR-DRBG do mbedTLS, semeado no main no boot) ->
PTK -> msg 2 com o IE RSN; msg 3 -> confere ANonce, MIC, replay, desembrulha a
GTK (AES key unwrap) -> msg 4 -> instala TK + GTK; rekey de grupo (1/2 -> 2/2 +
GTK nova). Senha errada aparece como o AP calado depois da msg 2: 7 s depois
o driver desiste ("Senha incorreta"); se a conexão veio da tela, não insiste;
rede salva tenta 3 vezes.

## Threads

- **hw1 (rede):** `xw_sys_step` roda no `net_hook`, logo depois do `usb_do_poll`
  (1 kHz), dentro do mesmo `if` (nunca durante o `xc_usb_hold`). USB, driver,
  lwIP (`netif_add`, `ethernet_input`, `dhcp_start`, `netif_set_default`,
  `etharp_tmr` a cada 5 s enquanto a netif `wl` existe, `np_dns_reload`).
  O `linkoutput` do lwIP (que pode rodar no `net_step`, até durante um hold) só
  copia pra fila; o envio ao USB é no `xw_sys_step`.
- **main:** lê o estado (`xw_sys_status`, cópia sob lock), faz pedidos
  (procurar/conectar/esquecer), PBKDF2 da senha (~4096x2 HMAC-SHA1, uma vez por
  senha digitada), grava/apaga `WIFI.TXT` (`xc_store_*`, que já fazem o hold).
- Nada nas threads do decoder. Nenhum lwIP fora da hw1.

## Boot com rede salva e sem cabo

`network_init` falha o DHCP do cabo (2,5 s) como sempre; `xw_sys_init` lê o
`WIFI.TXT`; a thread de rede solta o adaptador, sobe o firmware e conecta
sozinha. Enquanto isso (`xw_sys_boot_wait`, até 25 s) o menu mostra
"Conectando ao Wi-Fi <SSID>" e **segura** a validação da chave beta, a checagem
de atualização, o login e o worker das capas. `B` pula a espera, `X` abre a
tela do Wi-Fi. Quando o DHCP do Wi-Fi fecha, o `wl` vira a rota padrão e o DNS
é refeito; o boot segue normal.

Sem conexão na chave beta (sem cabo, sem Wi-Fi salvo), a tela de erro tem
`X:Wi-Fi` quando o adaptador existe: conecta, `B` volta, `A` tenta de novo.

## Tela

Configurações -> botão **Wi-Fi** (esquerda/direita escolhe; `X` continua
trocando o vídeo). Esquerda: estado (Cabo de rede / Wi-Fi: SSID com IP e sinal
/ Conectando... / Desconectado / erro), rede salva, **Procurar redes**,
**Esquecer rede**. Direita: redes (barras de sinal, cadeado, Conectada / Salva
/ Não suportada), 6 visíveis com rolagem. `A` conecta (aberta: direto; salva:
com a PMK salva; WPA2: abre o teclado), `Y` procura, `B` volta.

Teclado da senha (`xtk_*`): abc / ABC / #+= (todo ASCII imprimível), Espaço,
Apagar, Mostrar/Ocultar, Conectar (8 a 63 caracteres). `B` apaga (vazio:
volta), `Y` maiúsculas, `X` mostra, `Start` conecta. Mockups:
`hosttest/ui/out/41_` a `52_`.

A rede é gravada em `XC360/WIFI.TXT` quando o link sobe (`xw_sys_main_poll`):

    XCWIFI1
    ssid=<SSID em hex>
    pmk=<64 hex> | open

## Testes

    app/h264play/hosttest/wifi/run.sh    # também roda no hosttest/run.sh

- cripto: PBKDF2 (802.11 J.4: `password`/`IEEE`, `ThisIsAPassword`/`ThisIsASSID`),
  PRF-512 (J.3), AES key wrap/unwrap (RFC 3394 4.1, adulterado recusado), e 20
  PMK/PTK aleatórias conferidas com `hashlib`/`hmac` do Python;
- protocolo: bytes do probe/MRVL, comandos (tamanhos, TLVs, layout das chaves
  v1/v2), scan com 9 tipos de rede (aberta, WPA2, transição, SAE, PMF
  obrigatório, 802.1X, WEP, WPA1, grupo TKIP), truncado, SNAP -> Ethernet II;
- driver inteiro contra um firmware falso (ROM -> MRVL -> INIT_DONE, ou já
  rodando) + AP WPA2 falso (autenticador do 4-way e do rekey): senha errada
  desiste, senha certa conecta e a TK/GTK instaladas são as do AP (v1 e v2,
  grupo CCMP e TKIP), dados nos dois sentidos, rekey, DEAUTH -> reconecta,
  esquecer não reconecta, aberta, rede salva com senha que virou aberta,
  rede sumida tenta de novo, ROM sem INIT_DONE = falha do adaptador;
- `WIFI.TXT` (ida e volta, CRLF, inválidos), teclado de texto (todo ASCII,
  navegação entre fileiras de ações, limites, wipe);
- UI: mockups 41–52 com checagem de sujeira (parado não redesenha, pontinhos
  só no retângulo, incremental == completo).
- ASan + UBSan em tudo do host.

O que **não** dá pra testar sem um Slim: o USB de verdade (EHCI -> OHCI, bulk
full speed no OHCI do Xenon), o firmware real e um roteador real.

## Primeiro teste no console: o que olhar no log

`grep "\[wifi\]"` no log enviado:

| última linha | significado / próximo passo |
|---|---|
| `adaptador 045e:0765 não encontrado` | não enumerou (Fat, ou o EHCI não viu): ver as linhas `enum`/`ehci bus` do `[gip]` |
| `045e:0765 no EHCI b porta p`, sem `no OHCI` depois | a troca de dono da porta não reenumerou em full speed (o adaptador pode não aceitar FS) |
| `endpoint ...` / `faltam endpoints` | descritores em full speed diferentes do esperado (EP1/EP2) |
| `perguntando se o firmware...` e `sem resposta ao probe` | o EP1 não responde no OHCI: ver `usb_err` / tipo do endpoint |
| `modo ROM: mandando "MRVL"` e `sem INIT_DONE` | boot da flash falhou |
| `firmware x.y.z.pN ... API de chave` + `adaptador pronto` | init ok |
| `scan: N redes` | scan ok (N = 0 com rede por perto: TLV de scan) |
| `associação recusada: ... status S` | roteador recusou (S = código 802.11) |
| `esperando o handshake WPA2` e `handshake WPA2 não terminou (0 quadros EAPOL)` | EAPOL não chega (RX de dados, ou o firmware filtrando) |
| `EAPOL 1/4 -> 2/4` e depois nada | senha errada (o AP não manda a 3/4) ou o MIC/IE RSN do nosso lado |
| `firmware recusou a chave` | formato do `KEY_MATERIAL` (v1/v2) |
| `conectado em ...` sem `DHCP:` | dados não passam depois das chaves (TxPD, chave, ARP/DHCP) |
| `DHCP: a.b.c.d gw ...`, `rota padrão agora é o Wi-Fi` | tudo no ar; `[wifi] stat` a cada 60 s (rx/tx/drops/rssi) |

## Riscos conhecidos

- **Full speed:** o 88W8786U precisa funcionar em FS no OHCI (todo USB 2.0
  precisa enumerar em FS; funcionar inteiro em FS é o que não foi provado). Se não
  funcionar, o próximo passo é um driver EHCI assíncrono próprio (QH fixo com
  fila de qTDs) em vez de usar o tinyehci.
- **Banda/latência:** ~6–8 Mbit/s úteis e mais latência que o cabo (USB 1 ms +
  sem 11n). O vídeo pedido é 1,5 Mbit/s; capas e atualização ficam mais lentas.
- **`xc_usb_hold`:** o login/sessão gravando o pendrive (token, capas) para o
  USB, e agora também o Wi-Fi (ms a ~100 ms). No jogo isso só acontece no
  re-login (401).
- **TDs do OHCI:** 64 por controlador, divididos com controles/RF. O Wi-Fi usa
  até ~9 em voo; sem TD o envio espera o próximo passo.
- **Sem 11n:** roteador só-N (greenfield) recusa a associação.
- **Firmware rodando de boot anterior** (reinício quente): o probe trata, mas
  um estado associado antigo não é desfeito antes do primeiro `ASSOCIATE`.
- **Rotas:** com cabo sem DHCP o `en` continua com o IP estático do libxenon
  (192.168.1.99); o `wl` fica na frente da lista do lwIP, então a mesma sub-rede
  sai pelo Wi-Fi, mas um destino 192.168.1.x numa LAN Wi-Fi diferente sairia pelo cabo.
- **Build `BETA=0`:** a engrenagem das Configurações só existe com chave beta;
  na versão pública o Wi-Fi precisa de outra entrada (não feito).

## v0.21.1 — primeiro teste real (Slim Corona) e o caminho high speed

**O que o log da v0.21.0 mostrou** (console `ed4e…`, 2026-10-26 18:34–18:35, cabo
ligado): `045e:0765 no EHCI 1 porta 4`, depois (tela aberta) `porta entregue ao
OHCI (full speed)` e mais nada do Wi-Fi; a tela ficou em "Ligando".

**Diagnóstico no código:**
- Depois do `PORT_OWNER`, a v0.21.0 só chamava `find_dev()` a cada 100 ms, sem
  prazo e sem log. Também o `attach()` que falhasse deixava `dev = OFF`. Nos dois
  casos a tela mostra "Ligando" pra sempre, e o log não diz em que passo parou.
  Esse é o bug nosso, não importa o hardware.
- O log não prova que a porta 4 do EHCI 1 tem companheiro OHCI nem que ele
  enumerou. O único resumo `[gip]` do log é de antes da troca. O libxenon não
  liga nada disso: o OHCI só vê o que o hardware roteia pro root hub dele
  (`ohci_roothub_statchg`), e a v0.21.0 não registrava `HCSPARAMS`
  (N_CC/N_PCC), nem PORTSC depois da troca, nem as portas raiz do OHCI.
- Conclusão prática: full speed pelo companheiro ficou sem prova, e a evidência
  disponível é de que não funcionou no Corona. A v0.21.1 não depende mais dele.

**Mudança: high speed no EHCI primeiro** (`source/wifi/xw_ehs_xenon.{h,c}`).
O adaptador fica onde o boot o achou, com o endereço que o tinyehci deu
(porta+1, marcado `busy`: o `USBStorage_Init` não mexe). Os nossos QHs entram na
fila assíncrona daquele controlador:

    async (cabeça do tinyehci) -> EP0 -> EP1 IN -> EP1 OUT -> EP2 IN -> EP2 OUT -> async

- QH/qTD/buffers na memória não cacheada (alias `0xA0000000`, DMA = endereço &
  `0x1FFFFFFF`), little-endian, Active escrito por último depois de `sync`.
- Cada cano é um anel circular de qTDs com no máximo n-1 em voo: o HC para no
  primeiro inativo e retoma quando ele é ativado (EHCI 4.10.2). RX sempre armado
  (EP1 IN 3x2 KB, EP2 IN 7x4 KB); TX com ZLP quando o tamanho é múltiplo do MPS;
  Halt no overlay é registrado e o QH reiniciado.
- EP0 síncrono só na abertura: `GET_DESCRIPTOR(config)`, endpoints da
  interface 0, `SET_CONFIGURATION`.
- O tinyehci só faz transferência num controlador quando mexe num pendrive
  NESSE controlador. No Corona do usuário o pendrive está no EHCI 0 e o Wi-Fi no
  EHCI 1. Se um dia ele usar o mesmo controlador, ele reescreve `async->hw_next`;
  o `xhs_poll` (hw 1, fora do `xc_usb_hold`) confere a cada passo e religa
  (`religa=` no stat).
- Se o high speed falhar, cai pro full speed da v0.21.0, agora com log e prazo.

**Etapas com log e prazo** (`xw_xenon.c`), todas visíveis na tela ("Etapa: …
(N s)"):

| etapa | log | prazo |
|---|---|---|
| procurar no USB | `ligando o adaptador (EHCI: sim/não, OHCI: sim/não)` | 3 s pra achar no EHCI |
| abrir em high speed | `EHCI b: n portas, N companheiros x M portas, roteamento, 64 bits, portas: <PORTSC...>`, `HS: EHCI b porta p endereço a, porta <PORTSC> (...)`, `HS: GET_DESCRIPTOR`, `HS: endpoint ...`, `HS: SET_CONFIGURATION`, `HS: canos no ar` | 600 ms por transferência de controle |
| full speed (só se o HS falhar) | `high speed falhou: <motivo>`, `porta EHCI b/p entregue ao OHCI; porta agora <PORTSC>`, e por 8 s `OHCI ainda sem o adaptador` + `hcN ctl=... rh <portas raiz>` | 8 s |
| firmware | `-> EP1 n B: <hex>` / `<- EP1 n B: <hex>` (só os 10 primeiros, antes do "pronto": probe, MRVL, init; nada de chave), `etapa d (ms): HS rx_cmd=... erros=... halts=...` e `HS tokens EP1 IN ov=... td=...` a cada 2 s | probe 3x2,5 s, boot 10 s, total 30 s |
| falha | `[wifi] FALHOU depois de N ms: <motivo> (high speed: <motivo>)` | — |

A tela de Wi-Fi, a nota do botão nas Configurações e a espera do boot mostram o
motivo. Sem retry automático do adaptador: "Reinicie o Xbox".

**Como ler o próximo log:** se o HS funcionar, aparecem `HS: canos no ar`,
`-> EP1 12 B: ce fa 0d f0 a9 00 08 00 ...` (probe), depois `<- EP1 8 B: 01 00
00 00 ...` (ROM) ou `<- EP1 ... ce fa 0d f0 a9 80 ...` (firmware rodando) e
`adaptador pronto`. Se o EP0 não responder (`HS: GET_DESCRIPTOR(config, 9) -> -2/-3`),
o PORTSC da linha `HS: ...` diz se a porta está habilitada e com quem; `-2` =
Halt (o dispositivo recusou / erro de transação), `-3` = sem resposta em 600 ms
(fila assíncrona não passou pelo nosso QH: ver `cmd`/`sts`).

**Riscos novos do HS:** a fila assíncrona do EHCI 1 passa a ter 5 QHs nossos (o
HC fica fazendo IN com NAK nos endpoints de RX: banda de barramento, não CPU);
um EP1 IN do tipo interrupt é lido pela fila assíncrona (funciona em HS, mas não
é o periódico); se o tinyehci usar o EHCI 1 (pendrive numa porta desse
controlador), os nossos QHs saem do anel durante a transferência dele e
voltam no passo seguinte; nada disto foi testado em hardware.

## v0.21.4 — segundo teste real: o high speed funciona, o WPA2 não fechou

**Log da v0.21.1** (Corona, cabo ligado, 19:32–19:35): descritores, `SET_CONFIGURATION`,
canos no ar, ROM -> MRVL -> `INIT_DONE`, firmware 43.12.135.p3 (cap `0x00000bf3`,
sem supplicant no firmware, API de chave 1.0), `adaptador pronto`, `scan: 3 redes`.
O caminho high speed está provado. Depois disso, ~1500 linhas por lote de
`[wifi] HS rx_cmd=…`, e a tela disse "senha incorreta" com a senha certa.

**Achados:**
- A inundação era bug nosso: a linha de contadores "pronto e sem DHCP" nunca
  atualizava o relógio, então saía a cada passo (1 kHz) e empurrou do buffer as
  linhas da associação e do EAPOL.
- Os contadores finais dizem o essencial: `rx_dados=5` (chegaram dados pelo EP2
  IN 0x82, então o caminho de RX de dados do firmware funciona) e `tx_dados=0`
  (nunca saiu nada nosso pelo EP2 OUT: nem msg 2, nem msg 4), com `descartes=0`.
- "Senha incorreta" saía de qualquer fim de prazo do handshake ou de qualquer
  DEAUTH durante ele, sem prova nenhuma de senha errada.
- O mwifiex lê os dados do EP2 IN e manda dados pelo EP2 OUT, sem prefixo
  (conferido em `usb.c`/`txrx.c`). O 0x83 IN não é usado no Linux. O
  `MAC_CONTROL` (RX|TX|Ethernet II) já ia no init, com resultado 0.

**Mudanças:**
- **Log:** contadores do HS só quando mudam, no máximo 1 linha a cada 10 s.
  Cada passo aparece uma vez:
  - `associando (WPA2-PSK): canal, cap, taxas, IE RSN (grupo), comando N B` e depois `associado (aid…)` ou `associação recusada: resultado, status`;
  - `dados RX n B (estado): RxPD tipo, offset, tamanho, ethertype` (os 24 primeiros pacotes antes do link);
  - `EAPOL recebido: versão, tipo, descritor, key info (msg 1/4 | msg 3/4 | grupo)`, `EAPOL 1/4 -> 2/4 (n B na fila do USB, envio ok/RECUSADO)`;
  - `EAPOL saiu pelo USB (EP2 OUT concluído)`;
  - `EAPOL 3/4 -> 4/4` ou `MIC da msg 3 não confere`;
  - `chave instalada no firmware (par/grupo)` ou `firmware recusou a chave`;
  - `evento DEAUTH/DISASSOC (motivo N)`;
  - `falha: <texto> — <detalhe> (código)`.
  Nada de chave, PMK ou PTK.
- **"Senha incorreta" só com prova:** MIC da msg 3 errado, ou o AP repetindo a
  msg 1 / derrubando com motivo 14 ou 15 **depois** de a nossa msg 2 ter saído
  pelo USB (conclusão do EP2 OUT contada no glue: `xw_note_data_tx`). Sem prova
  é `XW_E_HANDSHAKE`: "O handshake WPA2 não terminou: <onde parou>; msg1
  recebida N, msg2 enviada N (saiu pelo USB N), msg1 de novo depois N, msg3 N,
  MIC ruim N". Isso aparece na tela e no log.
- **Corrida EP1/EP2:** se a msg 1 chega pelo EP2 antes da resposta do ASSOCIATE
  pelo EP1, ela é guardada e processada assim que a associação confirma (antes era
  descartada).
- **Defesa:** o 0x83 IN (quando existe) fica armado e o que chegar ali vai pro RX
  de dados (logado). Pacote do EP1 com prefixo `0xBEADC0DE` também é tratado como
  dados (logado). `rx_ep3=` no stat.
- **Teclado:** ao conectar, o log mostra só contagens da senha (tamanho,
  minúsculas, maiúsculas, dígitos, espaços, símbolos) e o tamanho do SSID. Um
  erro de mapeamento do teclado aparece ali sem expor a senha.
- **Testes:** o AP falso agora age como um de verdade: repete a msg 1 a cada 1 s,
  3 vezes, e derruba com motivo 15. Casos novos: msg 2 presa no USB =
  `XW_E_HANDSHAKE` (não "senha"); MIC da msg 3 errado = senha errada na hora;
  msg 1 antes da resposta do ASSOCIATE conecta. 772 verificações.

**Como ler o próximo log** (`grep "\[wifi\]"`):

| se aparecer | quer dizer |
|---|---|
| `dados RX … RxPD tipo 0x0000 … ethertype 0x888e` e depois `EAPOL recebido … msg 1/4` | a msg 1 chega e é entendida |
| `dados RX … tipo` diferente de 0 / ethertype diferente de 0x888e e nenhum `EAPOL recebido` | o firmware entrega a msg 1 num formato que descartamos: o RxPD está no log |
| `EAPOL recusado (-2: formato…)` | o descritor ou a versão do EAPOL não é o esperado |
| `EAPOL 1/4 -> 2/4 … envio ok` e nunca `EAPOL saiu pelo USB` | o EP2 OUT não completa: ver `HS tokens EP2 OUT` e `halts=` |
| `saiu pelo USB` + `msg 1 de novo depois` | o AP recusou o MIC da msg 2: senha (ou SSID/PMK) diferente |
| `associação recusada … status N` | o roteador recusou (código 802.11 N) |

## v0.21.5 — app só com Wi-Fi (sem cabo)

Feito sobre a v0.21.4 (que segue no console do usuário pro diagnóstico do WPA2;
quando aquele log chegar, o que ele ensinar entra numa próxima).

**Boot** (`source/wifi/xw_boot.{h,c}`, testado no host; o main chama a cada volta
até dar GO; até lá a chave beta, a atualização, o login e as capas esperam):

| situação | tela | sai quando |
|---|---|---|
| cabo com DHCP | nenhuma (como sempre; o adaptador nem liga) | — |
| sem cabo, adaptador ainda não visto (até ~8 s) | "Procurando o Wi-Fi interno" | achou / não achou |
| sem cabo, rede salva (até 30 s) | "Conectando ao Wi-Fi “SSID”", "Etapa: …", segundos | conectou (segue) ou falhou / 30 s (lista); `B` = lista |
| sem cabo, sem rede salva, ou a salva falhou | tela do Wi-Fi aberta direto (lista, senha, conectar) | conectou (fecha sozinha e segue); `B` = "Seguir sem internet" |
| sem cabo e sem adaptador (Fat), ou adaptador que não ligou | "Sem internet: conecte o cabo de rede" (+ motivo) | `A` = tenta o DHCP do cabo de novo (até 10 s); `B` = seguir sem internet |

"Seguir sem internet" cai no fluxo de sempre (a chave beta vale 7 dias sem
conexão). Depois do boot, tudo que usa rede (chave beta, atualização, login,
capas, sessão, log) sai pelo Wi-Fi: é o mesmo lwIP, com o `wl` como rota padrão
e o DNS refeito pelo DHCP do Wi-Fi.

**Cabo** (`tcp_stream.c`, `net_init_fast` no lugar do `network_init`): o
`enet_init` do libxenon continua igual (reset do PHY + até 5 s esperando o
link). Quem tem cabo precisa disso, porque o link volta uns segundos depois do
reset. A espera do DHCP mudou: sem link, nenhuma (antes 2,5 s); com link, até
10 s (antes 15 s). Sem cabo, o boot fica ~2,5 s mais curto (~5 s em vez de
~7,5 s). O DHCP do cabo continua em segundo plano; se fechar depois (cabo
plugado com o app aberto, ou o "Tentar de novo"), `stream_net_cable()` vira 1,
o boot segue e, se o Wi-Fi não for a rota, o DNS é refeito pelo cabo.
Log: `[net] cabo: link sim/não (PHY N ms), DHCP ok/sem resposta em N ms`.

**Adaptador:** sem cabo, liga logo no boot (antes só com rede salva). Com cabo,
nunca sozinho (regra 7).

**Log sem cabo:** o buffer do upload (200 KB em RAM) já guarda tudo até a chave
validar; com o Wi-Fi no ar o worker envia como sempre. Além disso, toda linha
`[wifi]` e `[net] cabo` vai pra `XC360/WIFILOG.TXT` no pendrive, com o tempo
desde o boot (`t=12.3 …`), começando com `===== boot orbita <versão> =====`.
O arquivo chega a 128 KB e então vira `WIFILOG.OLD` (~256 KB no total). Um anel de
32 KB em RAM junta as linhas (cheio: descarta as mais velhas e anota quantas). A
gravação acontece pelo main a cada 5 s, ou logo depois de uma decisão nova do
boot, só com a hw 3 livre (FatFs não é reentrante), ou pelo worker das capas
antes do lote do log beta. O `WIFI.TXT` também passou a ser gravado só com a
hw 3 livre. Nenhuma linha do Wi-Fi tem senha, PMK ou chave.

**Mockups:** `53_boot_conectando_wifi`, `54_boot_escolher_wifi`,
`55_boot_sem_internet`, `56_boot_procurando_cabo`, `57_boot_procurando_wifi`.

**Testes:** `test_boot` em `hosttest/wifi`: a tabela inteira (cabo x adaptador
-1/0/1 x falha x rede salva x Wi-Fi ok x desistiu x 0/29,9/30 s x pulou = 576
casos) contra uma especificação escrita à parte, mais os roteiros do pedido
(cabo; salva -> conectou; salva falhou -> lista -> conectou; salva fora do
alcance -> 30 s -> lista; sem salva -> pular; Fat -> sem internet -> cabo depois;
adaptador que não ligou). 786 verificações no total.

## v0.21.7 — travamento no menu com Wi-Fi, vigia e catálogo do pendrive

**O que aconteceu (v0.21.6, Slim sem cabo, 2026-10-26 21:28):** 4-way, chaves,
DHCP, chave beta, login e catálogo (1,25 MB) pelo Wi-Fi, `[capas] worker
ligado`, `SIGL ... (ok)`, e o menu parou de responder sem capas. O último lote
no R2 é das 21:29:00. No `WIFILOG.TXT` a última linha é `t=98.3` (o contador do
HS sai a cada 10 s quando muda, então em `t≈108` já não havia passo do Wi-Fi
ou ninguém gravando).

**O que estava errado no projeto (não no hardware):** o `xc_usb_hold` (v0.17)
parava o `usb_do_poll` inteiro na hw1 enquanto outra thread usava o pendrive.
O motivo era só o `USBStorage_Init` do tinyehci, que a hw1 chama a cada passo.
Com o cabo isso custava pouco. Com o Wi-Fi, a rede passou a depender desse
mesmo passo (nossos QHs no EHCI e o lwIP do `wl`), além dos controles (OHCI).
A partir do `worker ligado`, cada leitura ou gravação de capa no pendrive:
- parava controle e rede juntos;
- o menu ignorava os botões, porque o `menu_read_buttons` descarta tudo com o
  USB parado há mais de 40 ms;
- o download das capas (pela rede parada) emperrava.

Uma tarefa da hw 3 presa segurando o pendrive (nunca soltando o hold) travaria
o app inteiro. O log não mostra qual dos dois aconteceu; a v0.21.7 corrige os
dois casos e o vigia vai dizer.

**Correções:**
- **O hold só tira o tinyehci da hw1.** Durante um `xc_usb_hold`, a hw1 segue
  com:
  - o OHCI (`xw_usb_poll_no_storage`: o que o `usb_cfe_timer`, static no libxenon, faz, mais os ticks do GIP e do log), o que mantém controles, receptor e menu;
  - o Wi-Fi (`xw_sys_step_held`): só não procura/abre o adaptador (mexe nas estruturas do tinyehci) e não religa o anel assíncrono se houver pendrive no MESMO EHCI (`xhs_shared`). No Corona o pendrive está no EHCI 0 e o Wi-Fi no EHCI 1.

  Pendrive e Wi-Fi andam ao mesmo tempo, inclusive no jogo (o 401 que regrava o
  `TOKEN.TXT` não para mais o controle).
- **Gravações do main no pendrive:** o worker das capas quase nunca sai, então
  ele mesmo grava as pendências (`CATLIST.TXT`, `WIFI.TXT` de uma conexão nova,
  `WIFILOG.TXT`) entre os passos dele; o main só grava com a hw 3 livre.
  Ponteiro trocado atomicamente: nunca os dois.

**Vigia (`source/xc_wdog.{h,c}`):**
- Batimentos com rótulo por thread (pelo PIR):
  - main: `menu`, `jogo`, `energia…`;
  - hw1: `rede: laço (lwIP)`, `rede: usb_do_poll`, `rede: wifi`, `rede: USB sem o pendrive (hold)`;
  - hw 3: `capas: passo do worker`, `pendrive: lendo/gravando/acrescentando/apagando <arquivo>`, `pendrive: esperando a hw1 liberar o USB`, `rede: DNS <host>`, `rede: conectando/enviando/recebendo`.
- A hw1 confere o main e a hw 3 a cada 0,5 s; o main confere a hw1 a cada volta.
- A hw 3 só é vigiada com uma tarefa nossa nela (login, capas, atualização, chave, sessão). No jogo ela é do decoder.
- Parou por mais de 5 s: `[wdog] <thread> parada há N s em "<rótulo>" (USB do
  pendrive com: <quem>; visto por <quem>)`. De novo aos 15 s; depois
  `[wdog] … voltou depois de N s`.
- Destino das linhas: log normal, `WIFILOG.TXT` e o **`XC360/HANG.TXT`**, que
  não passa por FatFs nem pelo `xc_usb_hold`. No boot (main, antes da thread de
  rede) o arquivo é criado com 64 KB e conferido contíguo; cada registro é 1
  setor escrito direto no disco (`fat_diskio_raw_write`, buffer próprio). Os
  registros (`#N t=… unix=… <linha>`) ficam em anel e continuam do maior número
  do boot anterior, então o travamento de ontem não é apagado pelo boot de hoje.
- Escrita direta só com ninguém no pendrive, ou se quem segura é a própria
  thread parada há 15 s.
- Ler pelo FTP: `XC360/HANG.TXT` (o maior `#N` é o mais novo) e `XC360/WIFILOG.TXT`.

**Catálogo do pendrive:**
- Depois de cada login bom, a lista (id, nome, flags) vai pra
  `XC360/CATLIST.TXT`.
- No boot, ela entra no menu na hora (`[cat] lista do pendrive: N jogos`), logo
  depois da chave beta e da checagem de atualização. O login baixa a lista nova
  em segundo plano e troca quando chega. Jogar espera o login, com a mesma
  mensagem de antes.
- Antes do login, o worker das capas roda ~3 s só com o pendrive
  (`xcc_offline_for`): as capas do cache aparecem antes de qualquer rede.
  Depois o login sobe.

**Testes:** `test_wdog` (parada aos 5 s, sem repetir, 2º aviso aos 15 s,
voltou, hw 3 só com tarefa, escrita direta só sem ninguém no pendrive ou aos
15 s, main vigiando a hw1, hw2 fora) e o teste do catálogo com o vigia linkado.
Wi-Fi 798 verificações, catálogo 292, todos os outros passando.

**Não testado no console:**
- o OHCI sendo chamado pela hw1 enquanto a hw 3 usa o tinyehci (controladores
  e pilhas diferentes; a única coisa em comum era a regra antiga);
- a escrita direta do `HANG.TXT`;
- o aquecimento de 3 s das capas.

## v0.21.8 — o caminho de dados morre depois de ~1100 pacotes RX

### O que o console mostrou (v0.21.6 e v0.21.7, sem cabo)

Nas duas versões, o travamento veio logo depois do primeiro TX pesado (o POST do log
beta). Depois disso:

- `rx_dados` ficou parado em 1113–1115;
- `tx_dados` seguiu concluindo no USB (631–633 e subindo devagar);
- nenhum erro, halt ou descarte;
- HANG.TXT vazio.

O main e a hw 1 seguiam vivos: a rede não estava presa, a ponta de lá é que parou
de falar com a gente.

### Causa mais provável: ZLP no EP2 OUT

Ainda não confirmada no console; foi reproduzida no modelo do host.

O transporte HS manda um ZLP (pacote vazio) depois de toda transferência OUT
múltipla de 512 (mps). Com TxPD de 20, isso acontece quando o quadro tem 492 ou
1004 bytes.

- 492 = 54 de cabeçalho Ethernet+IP+TCP + 438 de dados. É o último segmento de um
  POST cortado em MSS 536, o caso do log beta.
- O firmware da flash do 88W8786U não espera ZLP no pipe de dados. O patch do
  Free60 já registra que "a bare packet wedges its pipe". Depois do ZLP ele aceita
  as transferências pelo USB mas não transmite mais nada no ar.
- Nossos ACKs/dados não saem, e o servidor só retransmite com recuo exponencial.
  O RX vira um "pingo" (1113 → 1115) e `tx_dados` continua andando. É exatamente o
  que o log mostrou.

### Correção

`mwx_txpd_offset` em `mwx_proto.c`: quando `(20 + quadro) % 64 == 0`, o quadro vai
no offset 24 em vez de 20. É o mesmo campo `tx_pkt_offset` que o mwifiex usa para
o pad de alinhamento, com os 4 bytes zerados.

- Nenhuma transferência de dados fica múltipla de 64, logo nunca múltipla de 512,
  e nunca sai ZLP.
- `xw_core.tx_padded` conta os quadros com pad. Aparece na linha `RX PARADO`.
- `xw_tx_nopad_test` (só testes) volta ao comportamento antigo.

### Estresse no host (`hosttest/wifi/stress`, roda dentro de `hosttest/wifi/run.sh`)

O teste usa o código real: `xw_ehs_xenon.c`, `xw_core.c`, `mwx_proto.c`,
`xw_wpa.c`, `xw_rxwatch.c` e o lwIP 1.4.1 do libxenon com o `lwipopts.h` do app,
contra modelos falsos de cada ponta:

- **EHCI falso**, com a semântica do anel assíncrono: overlay, o qTD inativo
  segurando a fila, regras de ZLP e NAK de 3% no OUT.
- **Dispositivo**, com fila de RX de 64, RxPD tipo 2 e SNAP tipo 0. Num ZLP no
  EP2 OUT ele "trava" o TX, como o firmware.
- **Par TCP**, que manda em massa segmentos de 6, 432, 944, 1456, 1460 e 300 bytes
  e misturados. Ele tem perda de 0,8%, duplicatas, ACK e RTO com recuo exponencial.
- **App** com a política do net_pipe: buffer de 256 KB e `ERR_MEM` quando o
  consumidor atrasa.

Roda em dois modos: determinístico (ASan+UBSan) e com o controlador numa thread
separada.

| Fase | O que faz | Verifica |
|---|---|---|
| 1 | volume, 120 mil RX (cerca de 123 mil transferências, 18–20 mil ZLPs de RX) | dados conferidos byte a byte; `pbuf_alloc` nunca falha; zero ZLP ou múltiplo de 64 no EP2 OUT; o pad é exercitado; o detector não dispara à toa; anéis consistentes |
| 2 | congela o EP2 IN no meio de uma transferência | o detector dispara uma vez, o rearme recupera e a transferência termina |
| 2b | código antigo (TxPD sempre 20) com o pedido de 438+54 bytes | sai ZLP, o "firmware" trava o TX e o detector vê (reproduz o console) |
| 3 | vazamentos | pool de pbufs e segmentos TCP voltam ao total |

O lado RX (ring, toggles, ZLPs do dispositivo, ERR_MEM) passou limpo em mais de
100 mil pacotes. Por isso o problema foi parar no TX.

### Detector de RX parado (`xw_rxwatch.c`, ligado em `xw_xenon.c`)

**Quando dispara.** O link está de pé, há TX concluído no USB e não chega nenhum RX
há 3 s, contados do primeiro TX sem resposta. Dispara no máximo uma vez a cada
10 s.

**O que faz ao disparar:**

- linhas `[wifi] RX PARADO` com as estatísticas HS;
- contadores do core: rx, tx, descartes, `tx_padded` e `pbuf_alloc falhou`;
- o dump de cada cano: prod/sub/cons/done, overlay cur/next/token e o estado:token
  de cada qTD;
- um `xhs_check` dos anéis.

Em seguida refaz os anéis de EP2 IN e EP3 IN, preservando o toggle, e conta as
recuperações (`xw_status_t.rx_rearms`). Se disparar de novo em menos de 60 s,
reassocia à mesma rede (`rx_reassoc`), o que reseta o TX do firmware.

### Vigia: main e rede paradas juntas

Até a v0.21.7, uma thread vigiava a outra. Se as duas parassem, ninguém via.

**Terceiro observador.** Agora existe o `xwd_observe()`, chamado:

- no laço do worker das capas;
- em todas as esperas do `net_pipe` (DNS, connect, send, recv, sleep).

Só age na hw 3 com tarefa ativa, a cada 500 ms. Quando main e rede passam de 5 s
paradas, grava `[wdog] main (hw0) e rede (hw1) paradas: ...`, de novo aos 15 s e
`voltaram` quando voltam.

**HANG.TXT antes do log.** O `xwd_report` agora escreve o setor do HANG.TXT antes
do log, porque o log passa pelo WIFILOG/beta, que podem depender de quem parou.

**Por que não o decrementador.** O vetor 0x900 do libxenon vai direto pro dump de
crash, e escrever no USB de dentro de uma exceção não é seguro.

**Lacuna conhecida.** No jogo a hw 3 é do decoder, e as threads de decode não
ganham trabalho. Nessa hora vale só a vigia mútua main ↔ rede.

### O que olhar no próximo log

- Se o pad resolveu: `tx_dados` e `rx_dados` sobem juntos depois do POST do log
  beta, e não há nenhuma linha `RX PARADO`.
- Se ainda aparecer `RX PARADO`: vale o dump dos qTDs (token com Active preso ou
  não) e se o rearme ou a reassociação recuperaram. Se nem a reassociação
  recuperar, o próximo passo é resetar o firmware.

## v0.21.9 — o RX parou de novo (sem ZLP): o que o log da v0.21.8 mostrou

### Fatos (R2 22:56:44 e 22:57:44, sem cabo)

**A teoria do ZLP caiu.** `TxPD com pad=0`: nenhum quadro precisou do pad, e o RX
parou de novo em `rx_dados=1116`. Nas versões anteriores tinha parado em
1113–1115. Foi sempre no fim do download do catálogo (~1,25 MB), sempre na mesma
carga do boot.

**O RX já piorava antes.** Um handshake TLS levou 4402 ms, contra 77–462 ms dos
outros. Logo depois veio `[wdog] rede (hw1) parada há 12 s em "rede: wifi"`. O
vigia é o main, e ele só viu aos 12 s (não aos 5), então o main também passou
esses segundos sem conferir.

**O que o dump do RX parado mostrou:**

- O EP2 IN tinha os 8 qTDs ativos, esperando, e o adaptador não mandava nada.
- O EP1 IN (eventos) ficou em `rx_cmd=12` a sessão inteira depois do init: o
  firmware não mandou nenhum evento. Por isso não houve ADDBA/BA. A associação é
  b/g, sem IE HT.
- Os tokens do EP2 OUT alternando 00008c01/80008c01 são só o bit de toggle de
  dados (bit 31) dos qTDs concluídos. É o normal.
- O rearme dos anéis não adiantou.

**No boot, a associação falhou na primeira tentativa.** Saiu
`comando 0x0012 sem resposta` seguido de "O roteador recusou a conexão
(código -1)", e a segunda tentativa, 3 s depois, deu certo. O boot inteiro levou
6077 ms medidos na hw1, menos que o próprio timeout de 12 s do ASSOCIATE. O
timeout disparou na hora e não depois de 12 s.

### Bug achado: o relógio do driver andava pra trás

O passo da hw1 pega o "agora" uma vez, no começo. Depois colhe o USB, e cada
resposta chega ao core com um `now_ms()` mais novo. O comando seguinte saía com
`t_sent` igual a esse agora mais novo. Em seguida, o `xw_tick` rodava com o
agora velho, e `now - t_sent` dava negativo, que em uint32 é enorme.

Resultado: timeout imediato do comando que acabou de sair. Era o ASSOCIATE no
boot: a resposta chegou atrasada e foi descartada em silêncio. Os EAPOL que o AP
mandou foram "ignorados, estado 6".

**Correção.** O relógio do core agora é monotônico (`clk()` em
`xw_tick`/`xw_on_cmd_in`/`xw_on_data_in`/`xw_start`). No host,
`test_v0219` roda com o relógio do RX 7 ms à frente e um firmware que leva 3 ms
pra responder. O código antigo falha logo no init. O novo conecta de primeira.

**Rótulo.** Tempo esgotado agora é `XW_E_FWTIMEOUT` ("O adaptador Wi-Fi demorou
a responder (tentando de novo)"), com o detalhe do comando. A mensagem "O
roteador recusou" ficou só para uma recusa de verdade, que vem com status 802.11.

**Respostas atrasadas** são logadas (`resposta atrasada do comando 0x....`) e
contadas.

### Outro bug: o EP3 IN morto desde o boot

O `halt_recover` apontava o overlay pro próprio qTD que tinha parado. Esse qTD
estava inativo, então o HC estacionava ali pra sempre. O EP3 IN errava no modo
ROM (XactErr), e o dump mostrava `cur == next` e `rx_ep3=1` desde a v0.21.4.

Agora o overlay vai pro primeiro qTD ainda ativo (ou pro "segurador" livre).

### A parada de 12 s da hw1 ainda sem causa provada

O código do passo não tem espera longa:

- o `ctrl()` síncrono, com timeout de 600 ms, só roda no open;
- as travas são curtas;
- o envio só copia pra fila.

O rótulo, porém, ficava em "rede: wifi" até a volta seguinte, mesmo com a hw1
parada no áudio, no controle ou no lwIP.

**Para o próximo log:**

- **Sub-passos com nome no vigia.** Agora há:
  - no `step`: pedidos, colhendo o USB (RX + lwIP), tick, vigia do RX, netif,
    ARP e publicação;
  - no `net_hook`: convert, áudio, depois do Wi-Fi, controle e fim.

  O aviso do vigia vira `em "rede: wifi / passo wifi: colhendo o USB (RX + lwIP)"`
  e diz há quantos ms o próprio vigia não conferia, o que mostra se o núcleo 0
  inteiro parou.
- **Passos lentos.** Todo passo de 150 ms ou mais gera
  `[wifi] hw1 lenta: passo "..." levou N ms`, no máximo uma linha por segundo.
  O passo mais lento sai na linha de 10 s.
- **Estado dos qTDs durante a parada.** Com a hw1 parada, o main lê os qTDs (só
  leitura, `xhs_peek`) e loga quantas transferências o HC concluiu e ninguém
  colheu. Assim dá pra saber se o adaptador continuou mandando durante a parada.
- **xc_log da hw1 sem UART síncrona.** Na UART a 115200, cada linha custava
  ~17 ms e o dump do RX parado ~0,3 s. Na hw1 a linha vai pra um anel de 16 KB,
  e o main manda 192 B por volta do menu. No jogo o main não drena; o que não
  couber some só da UART. Beta e WIFILOG seguem na hora.

### Observabilidade do firmware e do RX

- **Eventos.** Todo evento do EP1 é contado por id, e os 3 primeiros de cada id
  são logados (`evento do firmware 0x....`).
- **Comandos.** Todo comando depois de pronto, menos a varredura, é logado com o
  tempo de resposta. Os lentos (>= 1 s), os atrasados e o pior caso também são
  contados.
- **Linha de 10 s.** Traz:
  - o buraco máximo entre quadros de dados com o link de pé;
  - quantos buracos de 0,5 s ou mais (mostra o RX piorando antes de parar);
  - eventos e comandos;
  - ARPs de prova;
  - recuperações (reassociações / reinícios / com sucesso).

### Recuperação em degraus (glue, `rx_watch`)

0. **Prova.** Com o link de pé, DHCP feito e nada chegando há 1,5 s, sai um ARP
   pro gateway, no máximo um a cada 1,5 s. O detector vê o silêncio mesmo com o
   app parado.
1. **Detector.** Dispara com 3 s sem RX e TX saindo. Loga tudo, refaz os anéis
   de entrada e reassocia na hora, procurando só no canal atual
   (`xw_reconnect`, uma varredura de 1 canal).
2. **Reinício do firmware.** Se não houver RX de volta (link de pé e o RX andou
   depois disso) em 3 s:
   - `FUNC_SHUTDOWN` (0x00AA, como o `mwifiex_shutdown_drv`);
   - 300 ms depois, todos os anéis zerados;
   - `xw_init`/`xw_start`: probe, `FUNC_INIT`, `GET_HW_SPEC`, `MAC_CONTROL`;
   - reconexão à mesma rede no mesmo canal, com DHCP renovado pelo lwIP.
3. **Sem sucesso em 10 s.** Volta ao começo, e o detector tenta de novo.

A meta é o link de volta em ~5 s depois do disparo, se a reassociação resolver.
Com o reinício do firmware leva mais. Os contadores são publicados em
`xw_status_t`: `rx_rearms`, `rx_reassoc`, `fw_resets` e `rx_recovered`.

**Limite dos testes.** A máquina de recuperação mora no glue do Xbox. No host
estão testados `xw_reconnect` (1 canal), `FWTIMEOUT`, eventos e o relógio. O
reinício do firmware (`FUNC_SHUTDOWN` e depois `FUNC_INIT`) só o console prova.

### O que olhar no próximo log

- Se o boot ainda mostra `comando 0x0012 sem resposta`. Esperado: não mostra mais.
- Os valores de "RX buraco máx" e "buracos >= 0,5 s" durante o download do
  catálogo.
- Se aparecem `evento do firmware 0x....`, `resposta atrasada` ou
  `hw1 lenta: passo ...`.
- Se o vigia avisar de novo:
  - qual o passo;
  - se o main também estava sem conferir;
  - a linha `Wi-Fi durante a parada da hw1` (prontos sem colher ou esperando).
- Se o RX parar:
  - qual degrau recuperou (`recuperado pela reassociação` ou
    `recuperado pelo reinício do firmware`);
  - ou se nem o reinício trouxe o RX. Nesse caso o próximo passo é o reset de
    porta USB (o adaptador volta à ROM e faz o MRVL de novo).

## v0.21.10 — "Conectando vídeo" sem fim, log da sessão, PS e estatísticas do firmware

### O que a v0.21.9 mostrou (WIFILOG.TXT do pendrive)

- O boot ficou bom: conexão automática em 2,5 s e resposta do ASSOCIATE em 69 ms.
- O `rx_ep3=113` não significa EP3 vivo. São os XactErr do modo ROM, antes do
  MRVL, agora sem o cano estacionar.
- O RX não parou. De t=25,6 a 35,6 s chegaram 89 quadros, mas o app estava
  ocioso: o catálogo já tinha baixado e as capas vinham do cache. Os ARP de
  prova foram respondidos, e o detector não disparou porque não havia o que
  disparar.
- **O log morreu na sessão.** Só o worker das capas (hw 3, FatFs) gravava o
  WIFILOG. Na sessão a hw 3 é da sessão/decoder, então nada depois de t=35,6 foi
  gravado, e o R2 também parou (o envio beta é do mesmo worker). O HANG.TXT ficou
  vazio, o que indica que nenhuma thread vigiada ficou 5 s parada com o pendrive
  livre.

### Bugs achados no caminho da sessão sem cabo

1. **O candidato ICE saía com o IP errado.** `xcw_local_ipv4` lia o
   `netif.ip_addr` do Ethernet. Sem DHCP no cabo, o `net_init_fast` deixa o
   enet com o 192.168.1.99 de reserva, e o candidato host do WebRTC saía com esse
   IP em vez do Wi-Fi (192.168.15.x). Agora vale o IP da rota padrão
   (`netif_default`).
2. **As esperas do WebRTC na hw1 não rodavam o Wi-Fi.** `xcw_idle()`
   (`ports_sleep_ms` do libpeer) bombeia `stream_net_pump` = `net_step`. Isso
   inclui o `network_poll` do Ethernet, mas não o `xw_sys_step` (o RX do Wi-Fi,
   a ativação dos qTDs de envio, o DHCP). Durante essas esperas nada entrava nem
   saía pelo Wi-Fi.
   - Agora `stream_set_pump_extra(pump_wifi)` roda o passo do Wi-Fi dentro da
     bomba: mesma thread, com proteção contra reentrada e a variante "held"
     durante um `xc_usb_hold`.
3. **"Conectando vídeo" podia durar minutos.** Cada fase tinha o próprio prazo:
   offer 30 s, SDP 60 s, pre-SRD 8 s, mídia 17 s e primeiro vídeo 30 s, duas
   tentativas e cooldown. Somados, dava vários minutos de uma tela que parece
   travada.
   - **Prazo total na máquina da sessão.** `video_total_s=75` vale do
     PROVISIONED até o answer/ICE, por tentativa. Teste do host: com SDP sem
     answer e o prazo de fase em 600 s, desiste em menos de 120 s, com "Tempo
     esgotado conectando o video (SDP, 75s)".
   - **Rede na espera, no main.** Fora da fila, se o texto da sessão ficar
     parado:
     - aos 30 s: uma linha `[menu] espera: ...` com a idade do batimento da
       hw1 e o estado do Wi-Fi;
     - aos 120 s: `xn_stop()` e erro na tela ("Tempo esgotado em ... (Wi-Fi
       conectado / a rede do console parou)").
   - **Aviso na tela de espera.** Se a hw1 não bate há 8 s ou mais, a tela diz
     "A rede do console parou há N s (controle e Wi-Fi sem resposta)" em vez de
     parecer travada.

A causa exata do congelamento relatado não está provada: não há log da sessão.
Os bugs 1 e 2 afetam justamente essa fase sem cabo. Se a hw1 parar de verdade, a
tela agora diz isso, e o próximo log mostra onde (passos com nome do vigia).

### Log durante a sessão: XC360/NETLOG.TXT direto no disco

- **Arquivo.** É pré-alocado no boot: 512 KB, contíguo, conferido como o
  HANG.TXT.
- **Escrita.** Com sessão (o main avisa: `xw_sys_set_session(waiting ||
  g_native_game)`, e no jogo sempre), a hw1 grava o anel do WIFILOG em setores
  de 512 B direto no disco (`fat_diskio_raw_write`), um a cada 2 s, ou na hora
  com 2 KB acumulados.
- **Por que a hw1.** Ela é a dona do USB fora de um `xc_usb_hold`: sem FatFs,
  sem hold e sem depender da hw 3. Cada setor começa com `#N t=...`; o maior N é
  o mais novo (anel).
- **Linhas da sessão.** Na sessão, todas as linhas do app vão pro anel
  ([sess], [sdp], [ice], [xn], [tls], [menu] e falhas), até 12 por segundo de
  fontes ruidosas. [sess], [xn] e [menu] passam sempre.
- **A cada 5 s no jogo:**
  - a linha HS;
  - a linha de buracos;
  - `[wifi] sessão: link, DHCP, quadros, descartes, NETLOG N setores (falhas,
    pior us)`. O tempo de escrita no pendrive fica medido.
- **Como ler.** Pegue o NETLOG.TXT pelo FTP e ordene os setores por `#N`.

### Economia de energia e estatísticas do firmware

- **PS.** Depois de conectar: `PS_MODE_ENH` GET (bitmap da STA) e depois
  DIS_AUTO_PS (0x00FE, bitmap 0x10), com o resultado logado ("PS LIGADO/desligado",
  "desligada/recusado"). PS com o AP guardando quadros até o beacon daria
  exatamente buracos de 0,1–1,5 s. O Free60 diz que o firmware recusa ligar PS;
  aqui se vê o estado padrão.
- **Estatísticas** (a cada 5 s na sessão, 10 s fora):
  - `GET_LOG` (0x000B): tx, falhas, retry, multi-retry, dup, falha de ACK,
    rx_frag, FCS, beacons recebidos e perdidos;
  - `RSSI_INFO` (0x00A4): RSSI de dados e de beacon, ruído e SNR;
  - `TX_RATE_QUERY` (0x007F): taxa TX em Mb/s.

  Sai uma linha `[wifi] firmware: ...` 300 ms depois do pedido. Comando recusado
  ou sem resposta fica marcado e não é mais pedido. As consultas não poluem o
  log de comandos (só as lentas, de 200 ms ou mais).
- **Taxas.** O ASSOCIATE manda a interseção das taxas do AP com
  1/2/5,5/11/6/9/12/18/24/36/48/54. Não mandamos TX_RATE_CFG, então a taxa é a
  automática do firmware. A taxa real agora aparece no log.

### O que olhar no próximo teste (sem cabo, abrindo um jogo)

- No NETLOG.TXT:
  - as fases `[sess] fase ... -> ...`;
  - `[sdp] offer ... candidates=N` (agora com o IP do Wi-Fi);
  - `[ice]` e `[xcw] state`;
  - a linha de 5 s.
- `economia de energia: estado lido (... PS LIGADO/desligado)` e o resultado do
  DIS_AUTO_PS.
- Em `[wifi] firmware:`, se `retry`/`falhas`/`ack_falha` crescem junto com os
  buracos, a perda é no ar. Se não crescem, é no host.
- Se a tela de espera mostrar "A rede do console parou": o `[wdog]` com o passo
  da hw1.

## v0.21.11 — jogo pelo Wi-Fi: o anel do EP2 IN dessincronizado, reinício do firmware, vídeo perdido

### O que o NETLOG.TXT da v0.21.10 mostrou

- **O jogo rodou pelo Wi-Fi.** 59 fps, áudio ok, TX a 54 Mb/s, RSSI -30 dBm.
- **O ar está bom.** O GET_LOG deu retry 67 e ack_falha 76 em 1269 tx.
- **PS.** O GET do PS_MODE_ENH e o DIS_AUTO_PS voltaram com resultado 2: o
  firmware não tem o comando.
- **O RX parado é do HOST, não do firmware.** O dump do EP2 IN mostrou `prod ==
  cons` (o software achava o anel vazio, sem nada pendente), o HC estacionado no
  qTD livre (o "segurador") e os outros 7 qTDs **concluídos com dados**: status
  0x00 e 127/181/735/886 B recebidos. O colhedor só olha `state[cons]`. Com
  `cons` num slot livre, nunca mais colheu, e os quadros ficaram lá.
  - Primeiro caso: `0A..3A` concluídos, `4L..7L` livres, `prod=cons=4`.
  - Segundo caso: `0A..6A` concluídos, `7L`, `prod=cons=7`. Este aconteceu
    depois do rearme da v0.21.8, que reescrevia o overlay de um QH vivo, e o HC
    pode gravar o dele por cima.
- **A causa exata da dessincronia ainda não foi achada.** Pela leitura do código,
  todo `cons++` vem com um `prod++`, e só a hw1 mexe nos anéis.

### Correção 1: o software se acerta com o HC (`in_resync`, a cada poll)

**Quando dispara.** O HC está "estacionado": overlay sem Active e sem Halt, e o
qTD apontado pelo next do overlay (P) inativo. Isso só é normal com `P == prod`.
Fora disso, ou com `state[cons]` livre, ou com ativos ≠ n-1, há dessincronia.

**O que faz:**

- entrega, na ordem do HC (de P+1 até o último executado), os qTDs concluídos e
  não colhidos;
- rearma P+1..P+n-2 de trás pra frente e **P por último**: o HC está esperando
  nele e segue dali;
- o livre vira P-1, `cons=P`, `prod=P-1`;
- **não toca no overlay**, então não há corrida com o HC.

**Diagnóstico.** Cada anel guarda um rastro das últimas 16 operações (R colheu,
A armou, X refez, S ressincronizou, com slot/cons/prod). As 8 primeiras
ressincronias logam o estado antes e o rastro:
`[wifi] HS: EP2 IN fora de sincronia com o HC (#N): ...; rastro ...`. É isso que
vai achar a causa.

**No detector.** O detector de RX parado usa a mesma ressincronia (EP1 IN, EP2 IN
e EP3 IN) no lugar do rearme que reescrevia o overlay. Só depois reassocia.

**Estresse.** Na fase 1b, a cada ~400 ms o teste provoca o estado do console
(`prod = cons`) no meio de transferências em massa. Resultado: todas as
dessincronias foram acertadas, os dados conferidos byte a byte, o detector não
disparou e os anéis ficaram consistentes. No volume normal: 0 ressincronias.

### Correção 2: o reinício do firmware matava o Wi-Fi

Depois do `FUNC_SHUTDOWN` e do probe, o glue ainda media o prazo de 30 s do
"ligando" a partir do `g_t_act` do boot (85 s antes). Deu "o firmware não
respondeu ao probe" na hora, e o adaptador ficou marcado como morto
(`g_dead`). Agora o prazo recomeça no reinício.

A queda que levou ao reinício foi a dessincronia (o rearme deixou o anel fora
do lugar), que a correção 1 resolve em milissegundos, bem antes dos 3 s do
detector e dos ~7 s do consentimento do WebRTC. O reset de porta USB (volta à
ROM + MRVL) fica pra quando o reinício de firmware falhar de fato.

### Correção 3: vídeo perdido no jogo = tela parada

Quando o WebRTC morreu (consentimento ICE), o keepalive falhou e a máquina pediu
a thread da sessão. Só que no jogo a hw 3 é do decoder: "sessao esperando a hw3
(ocupada)" pra sempre, com a última imagem parada na tela (o "travou" relatado).

- `xn_video_lost_ms()` dá o tempo com o WebRTC fechado/falho, ou com a sessão
  esperando a hw 3 do decoder.
- O jogo sai com 5 s disso (`EXIT_NATIVE_LOST`): fecha o decoder (libera a
  hw 3), encerra a sessão e o menu mostra "A conexão do vídeo caiu (rede/Wi-Fi).
  A sessão foi encerrada: abra o jogo de novo."

### O que olhar no próximo teste

- `anéis ressincronizados N` na linha de 5 s e as linhas
  `fora de sincronia com o HC`. O rastro mostra a sequência que levou ao
  `prod == cons`.
- Se ainda aparecer `RX PARADO`, o anel estava no lugar e a parada é do
  firmware: reassociação, depois reinício com o prazo certo.
- Se o vídeo cair: o menu com o erro, não a imagem parada.

## v0.21.12 — congelou no começo do vídeo (v0.21.11): ressincronia conservadora e tripwire

### O que o NETLOG da v0.21.11 mostrou

- A sessão chegou ao ICE: `fase SDP -> ICE_BRIDGE` e `1 candidatos locais
  enviados` às 53,1 s. Depois disso não há mais nenhuma linha: o NETLOG, que a
  hw1 grava a cada 2 s, parou.
- O HANG.TXT não ganhou registro. Nem o main (que vigia a hw1) nem a hw1 (que
  vigia o main) escreveram: ou as duas pararam, ou o USB do pendrive ficou preso
  e as escritas diretas esperaram por ele.
- Até ali estava tudo limpo: `anéis ressincronizados 0` e nenhuma recuperação.

### Ressincronia (v0.21.11) e a corrida com o HC andando

A v0.21.11 agia na primeira leitura de "HC estacionado fora do segurador".
Com o HC andando rápido (o começo do stream), há uma janela em que a leitura
engana. O HC acaba X, o overlay fica inativo com next = X+1, e o X+1 termina
entre as duas leituras. O software via um "estacionado em X+1" que não existia
e rearmava qTDs que o HC estava usando. Isso perde dados e pode deixar o anel
de fato fora do lugar.

O travamento duro em si (hw1 e main) não ficou provado por essa via: os laços
eram limitados ao tamanho do anel e a função não segura lock nenhum. É, porém,
a única coisa nova na hw1 nesse ponto.

**v0.21.12.** A suspeita só vira ação se durar 100 ms ou mais com o mesmo P,
sem nenhum quadro colhido nesse tempo (`done` igual), e se o HC continuar
estacionado na reconferência feita na hora. Os laços continuam limitados a
`ntd`, sem lock. As suspeitas que somem sozinhas são contadas
(`xhs_resync_suspects`).

### Tripwire de travamento duro (`xwd_tripwire`)

Roda numa hw thread que não é nem o main nem a rede:

- **Menu e começo da sessão (até o decoder abrir):** uma tarefa pequena na hw 5,
  a cada 250 ms. O `run_video` devolve a hw 5 antes do decoder.
- **No jogo:** as esperas OCIOSAS das threads do decoder chamam o gancho
  `xpt_idle_hook` a cada 16 k voltas de espera, no `pthread_cond_wait` do
  xenon-pthread. Isso só acontece quando a thread não tem trabalho; o decode
  não ganha nada a fazer.

**O que reporta:**

- main ou rede paradas por 8 s ou mais, com o rótulo e o sub-passo de cada uma;
- o USB do pendrive preso por alguém há 8 s ou mais (nesse caso nem o NETLOG nem
  o HANG.TXT conseguem gravar).

**Como reporta (`xwd_trip_report` no main.c):**

- 1 setor no HANG.TXT, só se o pendrive não está com uma thread parada e se a
  rede não parou dentro do USB;
- o texto em vermelho no alto da tela (o fb do console, o mesmo do menu: com o
  main parado ninguém apaga);
- a UART.

Não passa pelo `xc_log`, que tem locks que a thread parada pode estar
segurando. O tripwire tem um TAS próprio e olha no máximo uma vez a cada 250 ms.

### Estresse: começo do stream

- **Fase 1c.** UDP de 1000–1400 B (como o RTP) a 4 por ms, durante 15 s, com o
  HC na thread separada no modo thread. No modo determinístico, a fila do
  "firmware" descarta como no ar.
  - Resultado: nada duplicado, fora de ordem ou cortado, nenhuma ressincronia
    falsa, detector quieto, anéis consistentes.
  - A mesma rajada com a dessincronia provocada a cada ~1 s: ressincroniza e
    segue em ordem.
- **Fase 1b.** Provoca a dessincronia a cada ~2 s, porque cada acerto agora
  espera os 100 ms.
- **Limite.** O HC falso não produz a janela da corrida sozinho (0 suspeitas
  novas na rajada). O congelamento da v0.21.11 não foi reproduzido no host.

### O que olhar no próximo teste

- Se travar: a linha vermelha na tela, e o setor `[wdog] TRAVOU: ...` no
  HANG.TXT ou na UART. Ela diz quem parou, em qual passo, e se o pendrive estava
  preso.
- `anéis ressincronizados N` na linha de 5 s, e as linhas
  `fora de sincronia com o HC` com o rastro.

## v0.21.13 — a hw1 presa em "colhendo o USB de novo" no começo do vídeo

### O que o tripwire da v0.21.12 pegou

```
#3 rede (hw1) parada há 5 s em "rede: wifi / passo wifi: colhendo o USB de novo" (visto por main)
#4 TRAVOU: rede (hw1) parada há 8 s ...; main (hw0) bateu há 3 ms (tripwire na hw3)
```

- O main estava vivo e o decoder já rodava (o tripwire veio da hw 3, numa espera
  ociosa do decoder). O primeiro quadro do jogo apareceu.
- A hw1 nunca saiu da 2ª colheita do passo do Wi-Fi, logo depois de "Created
  outbound SRTP session": o RTP começando.

### O que dá pra afirmar pelo código

- **A colheita não tem laço sem limite.** É um único `xhs_poll2` por vez. Cada
  anel colhe no máximo n qTDs, ativar os envios termina, e a ressincronia da
  v0.21.12 não age com o RX andando.
- **O tempo preso fica abaixo do driver**, dentro do `hs_rx`: driver, lwIP
  (`ethernet_input`, UDP/TCP), ou um lock tomado ali (o `xc_log` tem três). Até
  a v0.21.12 o sub-passo não descia além de "colhendo".
- **A bomba do `xcw_idle` (v0.21.10) aninha o passo do Wi-Fi.** Se uma espera do
  WebRTC acontecer DENTRO do caminho do RX, o colhedor e o lwIP são reentrados.
  O teste do host mostra o efeito no colhedor: quadros fora de ordem. Reentrar o
  lwIP (`tcp_input` e as globais estáticas dele) é a receita clássica de laço
  infinito numa lista corrompida.
- **A pilha da hw1 (128 KB) carrega tudo de uma vez:** WebRTC (DTLS, SRTP,
  usrsctp), a bomba, o passo do Wi-Fi e o lwIP. Logo abaixo dela ficam o anel de
  AUs e os ponteiros de função do `tcp_stream` (`g_idle_hook`, `g_net_extra`,
  `g_in_pump_extra`...). Um estouro ali trava ou corrompe sem tela vermelha.

### v0.21.13

- **O poll do EHCI nunca entra em si mesmo.** A reentrada é recusada e contada
  (`xhs_poll_reenters`).
- **O passo do Wi-Fi também não** (`g_step_reenter`).
- **Orçamento de 2 ms por poll.** Passou disso, para de colher; o resto fica pra
  próxima chamada e o resto da hw1 roda (sessão, WebRTC, controles, NETLOG).
  Conta em `xhs_poll_budget_hits`.
- **Nenhum qTD conta como progresso sem mudar de estado.** Só a colheita real
  (Active=0 no token, estado livre, `cons++`) e o rearme (estado ativo,
  `prod++`) mexem nos contadores.
- **Pilha da hw1 de 256 KB, pintada no início.**
  - A linha de 5 s da sessão dá o pico de uso e "ESTOUROU" se o canário de 1 KB
    do fundo foi tocado.
  - A mesma linha traz as reentradas (passo/poll) e as batidas no orçamento.
- **Sub-passos dentro do RX.** `wifi: RX dados -> driver`, `wifi: lwIP
  UDP/TCP/ARP/ICMP/IP/outro` e `wifi: lwIP voltou`. O vigia diz em qual
  protocolo do lwIP a hw1 parou.
- **Segunda linha do tripwire.** Quando a parada é da hw1, ela traz:
  - `RX colhido +N em 50 ms`: >0 = laço vivo, 0 = parada de vez;
  - o pico da pilha e se estourou;
  - as reentradas.

  Vai pro HANG.TXT e pra UART.

### Teste do host que falha na v0.21.12

Na fase 1c (rajada de UDP de 1000–1400 B), uma terceira rodada faz o RX chamar
o poll de novo de dentro do caminho do RX, como a bomba do `xcw_idle` faria:

- **v0.21.12:** 0 reentradas recusadas e centenas de quadros fora de ordem
  (442 no modo determinístico, 1113 no modo thread). FALHA.
- **v0.21.13:** todas as reentradas recusadas, nada fora de ordem nem
  duplicado, anéis consistentes.

O laço infinito em si não foi reproduzido no host. As linhas novas apontam onde
ele está, se ainda existir.

### O que olhar no próximo teste

- `[wifi] sessão: ... reentradas passo N poll N, orçamento N; pilha hw1 pico N
  KB de 256`. Reentrada maior que zero confirma o aninhamento; pico perto de
  128 KB confirma o estouro na pilha antiga.
- Se travar de novo:
  - o sub-passo no `[wdog] TRAVOU` (qual protocolo do lwIP);
  - a segunda linha (laço vivo ou parado, pilha).

## v0.21.14 — main e hw1 parados no mesmo instante: locks instrumentados

### O que a v0.21.13 mostrou

- O jogo rodou 10 s a 58,9 fps pelo Wi-Fi.
- Depois, o tripwire (hw 3, decoder) viu **main e hw1 parados no mesmo
  instante**: 8204 ms e 8206 ms.
  - A hw1 estava morta, não em laço: `RX colhido +0 em 50 ms`.
  - A pilha estava ok (25 KB de 256) e não houve reentrada.
- O sub-passo da hw1 era "colhendo o USB (RX + lwIP)". Os sub-passos de dentro
  do RX (`RX dados -> driver`, `lwIP ...`) NÃO apareceram, então ela parou dentro
  do poll do EHCI antes de entregar qualquer quadro. Ali só há leitura de
  qTD/QH, `xc_log` (raro) e nenhum lock próprio.

### Auditoria dos locks que o main e a hw1 tomam no jogo

| Lock | Quem | Dentro dele |
|---|---|---|
| sessão (`xs_lock`) | main (`xn_*`, `native_poll_state`), hw1 (`session_drive`, WebRTC) | só cópias e `snprintf`, nenhum log nem outro lock |
| net_pipe rx/tx (por cano) | hw1 (`on_recv`, `np_step`), hw3/main (`xcn_*`) | só cópias |
| wifi status | main (`xw_sys_status`), hw1 (`publish`, pedidos) | só cópias |
| WIFILOG, UART da hw1, log beta | todos (log) | **folhas**: nada é pego com elas seguras |
| mutex do xenon-pthread | hw1 (libpeer/mbedtls/usrsctp); main + decoder (ffmpeg) | não são compartilhados entre o main e a hw1 |
| `malloc` do libxenon | todos | interno (recursivo por PIR) |

**Ordem global** (documentada em `xc_lock.h`): sessão > net_pipe > wifi status >
folhas. Pela auditoria do código, nenhum par ABBA entre os locks do app.

### v0.21.14

**Locks instrumentados (`source/xc_lock.{h,c}`).** Cada lock guarda o dono (hw
thread), o lugar (arquivo:linha) e desde quando. Cada hw thread guarda qual lock
espera, de onde e desde quando. Cada par "segurando A, pegou B" fica registrado:
se alguém fizer B→A depois, conta como ordem invertida e guarda os nomes e o
lugar. Passaram por aqui:

- o `xs_lock`;
- o net_pipe rx/tx;
- o wifi status;
- o WIFILOG;
- o anel da UART da hw1;
- o log beta.

**Log que nunca espera.** As linhas de log só **tentam** (até 2000 voltas) e
descartam, com contagem, se o lock estiver ocupado: o `xc_log` da hw1, o
`xw_logfile_line` e o `xlu_line`. A hw1 no caminho do RX nunca fica presa num
lock de log.

**Tripwire.** Ganhou uma linha a mais (HANG.TXT, tela e UART):
`[wdog] locks: hwN espera "X" há N ms (pediu em ...); dono hwM desde há N ms
(pegou em ...) | seguros: ... | ordem invertida: ...`.

**Farejador de crash** (no hook do stdout, qualquer thread, sem lock). Os três
casos abaixo, vindos de qualquer thread, viram um setor `[wdog] CRASH na hwN:
...` no HANG.TXT e texto na tela:

- o `LWIP_ASSERT` do libxenon, que é `printf("\fline N in FILE"); while(1);`
  e trava a thread em silêncio;
- o `assertion ... failed` do newlib;
- o `[Exit] with code` do libxenon.

**Linha de 5 s da sessão.** Mostra a ordem invertida vista e as tentativas de
lock desistidas.

### Teste do host (`hosttest/locks`, ThreadSanitizer)

1. Duas threads (main e hw1 de mentira, `xlk_test_pir` 0 e 1), 300 mil voltas
   cada, nos padrões do app (sessão com log dentro, net_pipe, status, STAT e
   `[dc]` no log beta de verdade, `xc_logup.c`). Termina, sem ordem invertida e
   sem corrida no TSan.
2. O detector acha um ABBA feito em sequência.
3. O relatório diz quem espera qual lock, o dono e os dois lugares.

O deadlock do console não foi reproduzido: pela auditoria, os locks do app
estão em ordem. Se a próxima parada não mostrar espera num lock do app, sobram:

- uma trava fora deles (o lock do `malloc` do libxenon, um assert do lwIP, que
  agora aparece como CRASH);
- algo de hardware no núcleo 0, já que as duas threads paradas juntas (hw0 e
  hw1) são o mesmo núcleo.

### O que olhar no próximo teste

- `[wdog] locks: ...` no HANG.TXT: qual lock, dono e lugar. "Não espera nenhum
  lock do app" nas duas = não é lock nosso.
- `[wdog] CRASH na hwN: ...` (assert do lwIP/newlib, exit).
- Na linha de 5 s: `locks: ordem invertida ...; tentativas desistidas N`.

## v0.21.15 — hipótese: leitura de endereço inválido travando o núcleo 0

O main e a hw1 são as duas threads SMT do núcleo 0 e pararam juntos, com a hw1
dentro do poll do EHCI. Uma leitura de endereço físico inválido pode prender o
barramento e o núcleo inteiro, sem exceção.

### Auditoria do driver HS (`xw_ehs_xenon.c`)

**Endereços que o HC escreve** (cur/next/alt_next do overlay, next dos qTDs):
**nenhum é desreferenciado.** O `slot_of()` só **compara** o next do overlay
com o DMA dos nossos qTDs e devolve o índice ou -1. Não existe conversão
DMA→ponteiro no código do Xbox (só no teste do host, pro HC falso).

**Leituras do poll:**

- tokens dos NOSSOS qTDs e o overlay dos NOSSOS QHs, todos alocados aqui com
  `memalign` + alias não cacheado (`0xA0000000 | físico`);
- o `async->hw_next` do tinyehci, pelo ponteiro da struct dele (`g_h->async`),
  com `dcbf` antes.

**Registradores do EHCI:** só no open/diagnóstico (portas, `cmd`/`sts`), via
`g_h->regs` do controlador onde o `xhs_find` achou o adaptador. Não há leitura
de registrador no poll.

**alt_next / T-bit:** todo qTD é armado com `alt_next` = T (1). Com pacote
curto, o HC segue o `next`, que é sempre um dos nossos qTDs (fixo por slot).

**Overlay apontando pra um qTD de antes de uma reconstrução:** o `slot_of` não
acha e devolve -1, então a ressincronia não age. Os rearmes reusam os mesmos qTDs
(os ponteiros não mudam entre gerações).

**Conclusão:** no nosso código não há um caminho em que um valor do HC vire
endereço lido.

### v0.21.15

- **Rastro da última leitura.** Antes de cada leitura de memória que o HC
  escreve (token do qTD na colheita, token/next do overlay), a hw1 grava num
  estático o endereço físico, o que era e de qual cano. A 2ª linha do tripwire
  (HANG.TXT/UART) traz `última leitura da hw1 na memória do HC: <o quê> do
  <cano> em 0x<físico>`. Se o núcleo travar numa leitura dessas, o endereço fica
  registrado.
- **Menos leituras do HC na hora quente.** A ressincronia (next do overlay +
  token do qTD apontado) só roda com o cano de entrada sem colher nada há
  20 ms ou mais, no máximo a cada 10 ms (`xhs_overlay_reads` conta).
  - O teste de Halt continua em todo poll: 1 leitura do token do overlay.
  - Atrasar o Halt também (20 ms) deixou o estresse no modo thread instável, e
    o ganho era pequeno.
- **Estresse no modo thread.** Há uma instabilidade de partida que já existia e
  depende da semente: com alvo pequeno (3000), a semente 15 falha até no código
  sem o atraso. Nas sementes 11, 15, 21 e 31 com o alvo cheio (120 mil), tudo
  passa.

## v0.21.16 — qTD/QH dividindo linha de cache com o heap; chaves do DIAG.TXT

### O que a v0.21.14 mostrou

Travou em "Aguardando vídeo..." (STREAM_WAIT). No HANG.TXT:

- o main e a hw1 pararam com 5 ms de diferença;
- **nenhum lock do app** esperado ou seguro, nenhuma ordem invertida;
- a hw1 estava no poll do EHCI, antes de entregar qualquer quadro;
- o orçamento de 2 ms estourou 4 vezes (RX pesado).

### Bug achado: a linha de cache de 128 B

O `unc_alloc` fazia `memalign(64, 64)` pros qTDs e `memalign(64, 96)` pros QHs,
mas a linha de cache do Xenon tem **128 B**. Cada qTD/QH dividia a linha com o
cabeçalho do malloc ou outro bloco do heap.

O mecanismo:

- o heap (lwIP, mbedtls, libpeer, ffmpeg...) suja a parte dele da linha, pelo
  alias cacheado;
- quando a linha volta pra RAM, os 128 B inteiros são regravados com o que a
  CPU tinha no cache;
- o nosso qTD/QH, escrito pelo alias não cacheado e pelo HC, volta a um retrato
  velho: token Active de novo, overlay antigo.

Isso misturava o alias cacheado e o não cacheado da mesma linha física, que no
PowerPC é indefinido. Bate com os anéis "impossíveis" das versões anteriores
(todos os qTDs Active sem o adaptador mandar nada; o HC estacionado onde o
software não esperava) e piora com muito malloc, como no começo do stream.

**Correção.** Tudo que o HC lê/escreve tem alinhamento de 128 ou mais e tamanho
múltiplo de 128: linhas inteiras e só dele. Os buffers de 512/1024/2048/4096 já
estavam assim. O teste do host confere o alinhamento de cada QH/qTD.

Não há outro `dcbf`/`dcbst` em linha nossa além do `memdcbf` da alocação. O
`relink` faz `dcbf` + leitura no QH de cabeça do tinyehci, pelo ponteiro dele.

### Chaves do XC360/DIAG.TXT (bissecção sem recompilar)

`chave=valor`, uma por linha. O arquivo é lido no boot, antes do Wi-Fi, e a
linha `[wifi] DIAG.TXT ...` mostra o que valeu.

| Chave | Padrão | O que faz |
|---|---|---|
| `netlog=0` | 1 | sem escrita direta do NETLOG no pendrive durante a sessão (EHCI0 parado na sessão) |
| `overlay=0` | 1 | o poll não lê o overlay do QH: nada de teste de Halt nem ressincronia. Um Halt visto no token do qTD faz o software reescrever o overlay **sem ler** (inativo, sem Halt, apontando pro próximo armado; o toggle volta a 0) |
| `prio=1` | 0 | prioridade SMT média explícita (`or 2,2,2`) a cada volta da thread de rede e a cada chamada do hook |
| `dma128=0` | 1 | volta à alocação antiga de 64 B (só pra confirmar o bug acima) |

O teste do host roda o caminho de dados com `overlay=0` (meio MB em 2 conexões,
nenhuma leitura do overlay) e confere o alinhamento de 128.

### Prioridade SMT (hipótese b)

A hw1 roda na prioridade padrão (média). Só o `xc_polite_udelay` baixa
(`or 1,1,1`) e ele volta à média no fim (`or 2,2,2`). Não há `or 3,3,3`
(alta) no app, e não há espera apertada dentro do poll.

### NETLOG (hipótese c)

A escrita direta do NETLOG (tinyehci no EHCI0) acontece no hook, DEPOIS do passo
do Wi-Fi (sub-passo "rede: NETLOG"), nunca dentro de "colhendo o USB". A trava
viu "colhendo o USB (RX + lwIP)", então não estava no meio de uma escrita do
NETLOG. Mesmo assim, `netlog=0` deixa testar.

### Sugestão de ordem pros testes

1. v0.21.16 sem DIAG.TXT: a correção da linha de cache ligada.
2. Se travar: `overlay=0`, depois `netlog=0`, depois `prio=1`.
3. `dma128=0` só pra confirmar o bug (deve voltar a travar, ou a ter os anéis
   estranhos).

## v0.21.17 — a trava contra reentrada do poll ficou "ocupada" pra sempre

### O que a v0.21.16 mostrou

- Sem trava do núcleo: a correção da linha de cache valeu.
- 25 s de jogo a 58,9 fps, até t≈72 s. Dali em diante:
  - `reentradas poll` 7017 → 57741 → 92295;
  - `rx_dados` parado em 4751 e `tx_dados` parado em 1335;
  - nenhum Halt, erro ou recuperação, e o detector de RX parado nunca disparou.
- A trava do poll ficou "ocupada" e TODO poll foi recusado como reentrada. As
  reentradas do PASSO ficaram em 0, ou seja, não havia poll aninhado: a trava
  estava velha.
- **No mesmo instante** (t=72,6) o log mostrou `colhendo levou 2716500040 ms` e
  `tick do driver levou 72162 ms`. Esses números só saem se o `g_crumb_t`, outra
  estática da hw1 no .bss perto da trava (do outro lado do `g_core`), estava com
  lixo.
- O set/clear da trava tinha uma única saída. A explicação que sobra é
  **memória do .bss pisada** em volta das duas estáticas. Quem pisa não foi
  achado.

### v0.21.17

**A trava do poll (`pguard_t`).** Ocupa uma linha de cache de 128 B só dela,
com 4 canários antes e 4 depois. Guarda o dono (PIR), o quadro da pilha de quem
entrou (`__builtin_frame_address`) e desde quando.

- **Reentrada de verdade:** mesma thread com a pilha mais funda que a do dono.
  É recusada e contada.
- **Outra thread dentro há menos de 50 ms:** recusada.
- **Fora disso, a trava está velha:** é limpa e contada (`travas velhas`), com
  log alto `trava do poll VELHA ... limpando` (as 5 primeiras e depois 1 a cada
  1000).
- **Canário pisado:** log `MEMÓRIA PISADA em volta da trava do poll` com os
  valores encontrados. Os canários são restaurados.

**A trava do passo do Wi-Fi.** Mesma regra do quadro da pilha.

**Os sub-passos (`g_crumb`/`g_crumb_t`).** Ficam numa linha própria com
canários. Pisados: log e descarte da medição. Tempo andando pra trás: não mede.

**O detector de RX parado não depende mais do poll.** O TX que ele conta é o
TENTADO pelo core (`tx_frames + tx_drop`), não as conclusões do USB, que param
junto com o poll. Fora da trava, o `rx_watch` ainda vigia o próprio poll: sem
nenhum corpo de poll terminado há 1 s, ele loga `o poll do EHCI não termina há
1 s` com as reentradas, as travas velhas e as pisadas.

**Linha de 5 s da sessão.** Mostra travas velhas (poll/passo), pisadas e polls
completos.

### Teste do host que falha na v0.21.16

Na fase 1c, durante a rajada de UDP, o teste "esquece" a trava do poll ocupada
a cada ~700 ms (como no console) e, uma vez, pisa no canário com um dono
estranho:

- **v0.21.17:** 12 travas velhas limpas, 1 pisada vista, RX seguindo (16 mil /
  32 mil recebidos), nada fora de ordem.
- **Com a regra antiga (sempre recusar):** 694 recebidos, 0 limpas, e o detector
  não vê. FALHA.

A reentrada de verdade (o poll chamado de dentro do RX) continua recusada:
805/805 e 1611/1611.

### O que olhar no próximo teste

- `trava do poll VELHA` e `MEMÓRIA PISADA ...`. Se aparecerem, os valores dos
  canários dão pista de quem pisa (texto, ponteiro, contador...).
- Na linha de 5 s: `travas velhas poll N passo N, pisadas N; polls completos N`.

## v0.21.18 — canários nas duas pontas do g_core e do buffer de RX

**Pista do mapa de símbolos da v0.21.16.** O `g_crumb_t` (com lixo em t=72,6)
fica logo DEPOIS do fim do `g_core` (xw_core_t, 0x2efc B), e a trava do poll
logo ANTES do começo.

**Auditoria das escritas no xw_core_t.** Todas limitadas pelo tamanho do
destino, pelo código:
- `lb` (o último campo): `snprintf`/`vsnprintf` com `sizeof`;
- `txbuf`: offset + quadro <= `sizeof`;
- `pend_eapol`: `n <= sizeof`;
- `fws`: < 19;
- tabela de eventos: <= 16;
- lista de redes / `tmp`: < `XW_MAX_BSS`;
- SSID: `l <= MWX_SSID_MAX`;
- taxas: < `MWX_RATES_MAX`;
- `cbuf` / fila de comandos: `n <= sizeof`.

Não há índice negativo. A escrita fora do lugar não foi achada.

**Mudança.** O `g_core` e o `g_rx_copy` agora ficam numa caixa alinhada em
128 B, com 128 B de canário de cada lado (`0xC0DE0000 | lado<<8 | índice`).
- Os canários são conferidos no começo de cada passo do Wi-Fi.
- O primeiro estrago vai pro log:
  `MEMÓRIA PISADA antes do começo do / depois do fim do <bloco> (N B): palavras
  i..j do canário, a partir de X B ...; achado: <8 palavras>`.
  O lado diz se foi estouro pra frente (depois do fim) ou escrita pra trás /
  índice negativo (antes do começo). Os bytes achados dizem que tipo de dado
  pisou.
- Os canários são restaurados depois do log.
- A linha de 5 s conta os canários pisados.

## v0.21.19 — o núcleo 0 parava numa leitura não cacheada de um token nosso

**Achado (crumb da v0.21.15, duas travas).** As duas travas pararam main+hw1 na
LEITURA não cacheada de um token de qTD nosso, em endereço válido do pool:
`token do qTD (colheita) do EP1 IN em 0x0167e088` e `... do EP3 IN em
0x0167d508`. Nenhum canário foi pisado.

**Causa provável: o mesmo físico em dois aliases.** O libxenon mapeia a RAM
cacheada em 0x80000000 e guarded/não cacheada em 0xA0000000
(`drivers/ppc/vm.c`). Os nossos QH/qTD/buffers vinham do heap (`memalign`) e
eram usados pelo 0xA..., mas as MESMAS páginas físicas são heap pelo 0x8...:

- vizinhos, memcpy e prefetch podem levar a linha pro L2;
- uma leitura cache-inhibited de uma linha válida no L2 é indefinida no PowerPC;
- no Xenon isso trava o núcleo (as duas threads SMT).

**Como fazem os drivers que funcionam:**

- **tinyehci (pendrive):** pool próprio (`heapspace` estático), descritores e
  buffers pelo alias CACHEADO, `dcbst` antes de entregar e `dcbf` depois,
  síncrono (só lê o descritor depois da transferência).
- **OHCI com o patch do GIP:** pools pelo 0xA..., arredondados a 128 B, a 1 kHz.

**v0.21.19:**

- **Descritores (QH/qTD):** num **pool estático só deles** (32 KB, com 4 KB de
  guarda de cada lado, alinhado a 4 KB). É limpo do cache uma vez (`memdcbf`)
  e daí acessado só pelo alias não cacheado: o heap e os vizinhos nunca chegam
  nessas páginas.
- **Buffers de dados:** só pelo alias CACHEADO, como o tinyehci. `dcbst` antes de
  entregar ao HC (OUT, setup do EP0); `dcbf` depois que ele terminou e antes de
  ler (IN, colheita, ressincronia, dados do EP0). Nenhuma linha física é tocada
  pelos dois aliases.
- **`eieio` + `sync`** entre as escritas do qTD e o token.
- **EP3 IN desligado** (os dados vêm no EP2 IN 0x82): metade da exposição a menos.
- **Novas chaves do DIAG.TXT (bissecção):**
  - `dmapool=0`: descritores do heap, como antes;
  - `bufcache=0`: buffers não cacheados, como antes;
  - `ep3=1`: liga o EP3 IN.

  A linha `[wifi] DIAG.TXT ...` mostra os valores.

**Pra depois (não mexido aqui).** O MK11 com lag e o Fortnite preso no lobby:
vazão/latência pelo Wi-Fi com o orçamento de 2 ms por poll e o hook a cada
~1 ms (o `usb_do_poll`/Wi-Fi roda 1x por ms no `net_hook`), e as cópias.
Medir antes de mudar.

## v0.21.20 — controle com fio na porta da frente derrubava o Wi-Fi

### Como o libxenon trata uma conexão numa porta EHCI

O `usb_do_poll()` (1x por ms, na hw1) chama o `USBStorage_Init()` do tinyehci,
que varre os DOIS EHCIs, porta por porta:

- **Dispositivo já enumerado e sem `busy`:** `ehci_reset_port` (reset +
  GET_DESCRIPTOR + SET_ADDRESS = porta+1) e teste de mass storage.
- **Porta conectada sem dono:** `ehci_reset_port2`.
  - reset da porta, esperando o PE;
  - não habilitou (full/low speed) → `PORT_OWNER` (o OHCI companheiro enumera);
  - falhou → `msleep(500)` + desliga/liga a energia;
  - cada tentativa tem esperas de 10..600 ms **na hw1**.
- **As transferências de controle do tinyehci:** usam o `asyncqh` DELE e fazem
  `async->hw_next = asyncqh` e depois `async->hw_next = async`. Isso **tira os
  nossos QHs do anel** daquele controlador até o nosso `relink`.

Com o Wi-Fi no EHCI 1, um controle 360 com fio plugado numa porta desse
controlador fazia o tinyehci:

- parar a hw1 em esperas longas a cada poll;
- reescrever o anel do EHCI do Wi-Fi.

Sem HANG (a hw1 seguia batendo em "rede: usb_do_poll"), mas sem rede.

### v0.21.20

**O EHCI do Wi-Fi é nosso.** Ao abrir, e a cada 10 ms depois, TODAS as portas
desse controlador ficam `busy` pro tinyehci, que nunca mais reseta nem enumera
nada nele. O EHCI 0 (o pendrive) segue com o tinyehci como antes.

**Hotplug nas outras portas do EHCI do Wi-Fi.** `xhs_port_watch`, uma máquina de
estados sem bloquear. Só mexe nos registradores da PORTA: nunca no USBCMD, no
ASYNCLISTADDR ou no anel.

- **Baixa velocidade** (K no line status): `PORT_OWNER` direto.
- **Senão:** reset de 50 ms.
  - não habilitou = full speed → `PORT_OWNER` (o OHCI enumera, como no boot);
  - habilitou = **high speed** (pendrive na frente) → porta desabilitada e log
    ("use uma porta de trás pro pendrive"). Não dá pra enumerar sem mexer no
    nosso anel.
- **Desconexão:** volta a esperar (o HC devolve a porta). Ao religar, o mesmo
  tratamento.

**Log.** Toda mudança de PORTSC nos dois EHCIs:
`[usb] EHCI b porta p: antigo -> novo (conectado/habilitada/em reset/com o
OHCI/linha J|K|SE0)`. Depois de cada mudança no EHCI do Wi-Fi, a saúde do anel:
"com/SEM os nossos QHs" e quantos religamentos. A linha de 5 s mostra mudanças,
passagens pro OHCI e high speed bloqueado.

### Teste do host

O EHCI falso ganhou o modelo da PORTSC (RWC, reset, PE só pra high speed,
`PORT_OWNER`, devolução na desconexão) numa porta da frente. Com a rajada de UDP
do Wi-Fi rodando:

- conecta full speed → OHCI;
- desconecta;
- conecta baixa velocidade → OHCI;
- conecta high speed → bloqueado;
- religa full speed → OHCI de novo.

Resultado: o tinyehci fica fora, 3 passagens + 1 bloqueio, o RX do Wi-Fi segue
(12 mil / 24 mil quadros) sem nada fora de ordem, e os anéis ficam consistentes.

### O que olhar no próximo teste

- As linhas `[usb] EHCI ...` ao plugar o controle: em qual EHCI e porta ele
  aparece (as portas da frente podem ser do EHCI 0) e se foi pro OHCI.
- Depois disso, o `[gip]` enum com o controle com fio.
