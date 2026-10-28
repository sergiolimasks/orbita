# Configurações em menu lateral + botão Guia

Status (2026-10-27): **implementado na v0.21.33** (branch fase1-latencia, ainda sem
teste no console). Este arquivo segue como o plano; o que mudou na hora de fazer
está logo abaixo. Mockups originais (aprovados): `hosttest/ui/settings_v2.c` +
`run_settings_v2.sh`; as telas pelo código real: cenas `60_`–`69b_` do
`hosttest/ui/run.sh` (`hosttest/ui/out/`).

## Decisões aprovadas (2026-10-27)

- **D1:** Guia no jogo continua indo pro xCloud como Nexus (nada mudou no
  `send_pad`/`xc_proto.c`). No menu ela abre/fecha as Configurações pela tabela
  do §1. Sem "segurar Guia 1 s".
- **D2:** zona morta global, separada por analógico, `XC360/PAD.TXT`; padrão = os
  valores de compilação (STICK_DZ/STICK_OUTER/RSTICK_DZ).
- **D3:** Reiniciar/Desligar também na coluna (ENERGIA); os botões do topo ficam.
- **D4:** Configurações existem sem chave beta (BETA=0); a linha "Chave beta" some.
- **D5 (mudou):** seção **Vídeo e Som**; 720p **e** 1080p escolhíveis (sem "EM
  BREVE"). Escolher grava `VIDEO.TXT` pelo mesmo caminho do X de antes
  (`xcv_settings_set` + `xcv_save_pending`, vale no próximo boot, LB+RB no boot
  continua forçando 720p); o X saiu. Som: Volume 100/150/200/250% (esquerda/
  direita), na hora (`audio_set_volume_pct`) + `VOLUME.TXT`.
- **D6:** NOTES.TXT gravado na instalação + notas do manifesto quando a publicada
  = a instalada. **D7:** "Medir sozinho" incluído.
- Rede: "cabo conectado" = `stream_net_cable()` (DHCP do cabo), nunca o link do PHY.
- Botões: A confirma; B volta (painel -> coluna, subpágina -> seção, calibração =
  salva e volta) ou, na coluna, fecha; X nunca; Guia fecha tudo.

## O que ficou diferente do plano (v0.21.33)

- Arquivos: `source/settings/xc_cfgnav.{h,c}` (navegação), `source/settings/xc_cfgview.{h,c}`
  (estado achatado `xcs_in_t` -> `xui_cfg_t`, opções por seção, calibração
  `xcs_cal_*`), `source/settings/xc_notes.{h,c}` (NOTES.TXT), `source/input/xc_padcfg.{h,c}`.
  O `main.c` só preenche o `xcs_in_t` e executa as ações; o mockup do host usa o
  mesmo `xcs_build` + `xui_cfg_frame`.
- Ícones da coluna (conta, rede, tela) desenhados com primitivas, como no mockup
  aprovado (o gerador de assets não mudou).
- NOTES.TXT: `XCN1` / versão / hora Unix da instalação (`xc_time_now`, -1 se não
  sabe) / notas. "Instalada em" vem dessa hora.
- LB+RB segurados no boot também ignoram o PAD.TXT nesta vez (o arquivo fica).
- Vídeo: com a escolha diferente da saída atual, a linha escolhida mostra "AO
  REINICIAR"; 1080p apagado fora do HDMI ("só no cabo HDMI") ou sem tabela 1080p
  ("indisponível neste console"). Volume fora das 4 opções: "Agora: N%,
  personalizado (VOLUME.TXT)" embaixo da linha. A no Volume = próxima opção.
- Calibração: LB/RB não saem dela (só B/Guia, que salvam). "Controle N" aparece
  quando mais de um controle mexe. Medindo: A cancela.
- Caso de borda "cabo pegou DHCP no boot e foi tirado": não dá pra detectar (o
  PHY mente no Corona); o texto do cartão do cabo já diz "tire o cabo e reinicie".
- Trocar chave e o Wi-Fi abertos pelas Configurações: B volta pra seção; a Guia
  fecha tudo (no Trocar chave, só sem validação em andamento).

## Decisões que faltavam (proposta original; ver "Decisões aprovadas" acima)

| # | Pergunta | Recomendação |
|---|---|---|
| D1 | Guia **dentro do jogo**: continua indo pro xCloud como Nexus (hoje) ou o orbita pega? | Continua indo pro xCloud. Opcional: segurar Guia 1 s = voltar ao menu (além de Back+Start). |
| D2 | Zona morta: global, por tipo de controle ou por controle? | **Global, separada por analógico** (esquerdo/direito). "Por controle" não tem identidade estável (ver abaixo). |
| D3 | Reiniciar/Desligar também na coluna das Configurações? | Sim, embaixo, separados ("ENERGIA"). Os botões do topo da home ficam como estão. |
| D4 | Configurações sem chave beta (build `BETA=0`)? | Sim: a engrenagem e a Guia passam a existir sempre; a "Chave beta" some da seção Conta. |
| D5 | 1080p "em breve": e o modo automático/`VIDEO.TXT`? | A tela só deixa 720p. O `X` que troca o vídeo sai. `VIDEO.TXT` (pelo PC) e o fallback automático por EDID (TV sem 720p60) continuam, pros testes. |
| D6 | De onde vêm as "Novidades"? | Cache `XC360/NOTES.TXT` gravado ao instalar + as notas do manifesto quando a versão publicada = a instalada. |
| D7 | "Medir sozinho" na calibração já na primeira versão? | Sim (é pequeno, e testador leigo não sabe que número pôr). |

## 1. Botão Guia: o que existe hoje

**O libxenon já entrega o Guia** no campo `logo` de `struct controller_data_s`
(`~/libxenon-gip-src/libxenon/drivers/input/input.h`, instalado em
`~/devkitxenon/usr/include/input/input.h`). Não precisa mexer no libxenon:

| controle | caminho | onde |
|---|---|---|
| 360 com fio (`045e:028e`) | `usbctrl_ireq_callback` | `drivers/usb/usbctrl.c:416` `c.logo = !!(b[3] & 0x4)` |
| 360 sem fio pelo RF interno (`045e:0291`) e receptores `045e:02a9`/`02aa` | mesmo callback; o pacote sem fio (`b[5] == 0x13`) anda 4 bytes (`b += 4`) e cai no mesmo `b[3] & 0x4` | `usbctrl.c:~378-416`, ids em `usbdevs.c:97-100` |
| One/Series (GIP, nosso patch) | pacote `0x07` (`GIP_CMD_VIRTUAL_KEY`), separado do report `0x20`; `logo = p[0] & 3`, com ack | `drivers/usb/gip_proto.c:250-254`; publicado por `usbgip.c:369` (`GIP_EV_GUIDE`) |

O que falta é **só no app**:
- `get_any_pad` (`source/main.c:~504-520`) já junta `logo` dos 4 slots, mas
  `menu_btn_t` (`main.c:1713`) não tem o campo e `menu_read_buttons` (`main.c:1717`)
  não copia. Hoje a Guia não faz nada no menu.
- **No jogo a Guia já vai pro xCloud**: `send_pad` põe `logo` no bit 2 de `buf[2]`
  (`main.c:452`) -> `xcp_gamepad_from_wire24` vira `nexus` (`webrtc/xc_proto.c:119`)
  -> bit `2` da máscara do report (`xc_proto.c:69`). No menu nada vaza: `send_pad`
  só roda com `g_pad_forward` (`main.c:704`) e só manda com `g_native_game`.
- Back+Start 2 s (`ingame_check`, `main.c:971-982`) continua sendo a volta ao menu.

Cuidados:
- Controle 360 sem fio: segurar a Guia uns segundos é do próprio controle (desliga o
  controle). Usar só o **toque** (borda de subida), nunca "segurar".
- GIP: o estado da Guia fica travado até o pacote de soltar (`0x07` com 0); se o
  controle for desligado com a Guia apertada, o detach zera tudo (`usbgip.c:762`).
- `prev` é lido na entrada do loop do menu (`main.c:2239`), então quem volta do jogo
  com a Guia apertada não abre as Configurações por engano (só abre ao soltar e
  apertar de novo). Manter isso.

### Regra proposta da Guia

| onde | Guia faz |
|---|---|
| home (`SCR_LIST`), carregando (`SCR_LOADING`), falha do login (`SCR_FAIL`), código do login (`SCR_LOGIN`) | abre as Configurações na seção Conta e sobre (ou na última aberta nesta sessão) |
| qualquer tela das Configurações, inclusive calibração, Wi-Fi e senha do Wi-Fi abertos por ela | fecha tudo e volta pra home (calibração: salva como o B; senha: apaga o digitado) |
| no jogo | vai pro xCloud (D1) |
| iniciando/fila (`SCR_WAIT`), atualização instalando ou popup obrigatório, confirmação de energia, `XUI_ST_BUSY`, telas da chave beta sem autorização, boot do Wi-Fi | nada |

## 2. Desenho (mockups 60–68)

Tela inteira por cima do hero escurecido (mesmo fundo das telas de hoje), logo +
versão no topo. Esquerda: coluna "Configurações" (312 px) com as 4 seções e, embaixo,
"ENERGIA" com Reiniciar/Desligar. Direita: painel da seção (820 px). Mesma linguagem
da home: foco = pílula branca com texto escuro; seção aberta enquanto o foco está à
direita = pílula cinza com barrinha Orbita Sky; avisos em Flare; botões A/B/X/Y nas
cores do controle; Inter.

### Navegação

| | coluna (seções) | painel (opções) |
|---|---|---|
| ↑/↓ (D-pad ou analógico esq.) | troca de seção; o painel mostra a seção na hora | anda nas opções |
| → / A | entra no painel (1ª opção). A em Reiniciar/Desligar abre a confirmação de hoje | A abre/ativa a opção |
| ← / B | B fecha as Configurações | volta pra coluna |
| LB / RB | seção anterior/seguinte (de qualquer lugar, foco vai pra coluna) | idem |
| Guia | fecha tudo | fecha tudo |

Seção sem opção focável (Rede com cabo): → não faz nada, a dica mostra só B.

### Seções e textos

**Conta e sobre** (`60_cfg_conta`, `61_cfg_conta_atualizacao`, `62_cfg_novidades`)
- Blocos: "ID DO CONSOLE" `A1B2-C3D4-E5F6` + os 32 hex; "VERSÃO" `orbita v0.21.28` +
  "beta fechada · canal beta".
- Opções: **Chave beta** (`*****-**P9H · validada em 25/10 22:56` ›; A = Trocar chave,
  o fluxo de hoje com `g_bchange`, B volta pra cá); **Novidades desta versão** (`v0.21.28` ›);
  **Procurar atualização** ("você está na versão mais nova") ou, com versão nova, ponto
  Flare + **Atualizar para v0.22.0** ("nova versão disponível"): A abre a `SCR_UPDATE`
  de hoje.
- Caixa: "Para reportar um bug, envie uma foto desta tela junto."
- Novidades: título "Novidades da v0.21.28", "Instalada em 26/10/2026", texto das notas
  (quebra sozinho, ↑/↓ rola se passar). Sem notas: "As notas desta versão não estão
  neste console. Elas aparecem na próxima atualização."
- Dicas: coluna `A Entrar · LB/RB Seção · B Fechar`; painel `A Abrir · B Voltar · Guia Fechar`.

**Rede** (`63_cfg_rede_cabo`, `64_cfg_rede_wifi`)
- Cabo com internet (`stream_net_cable()`; o link do PHY diz "sem link" no Corona mesmo com
  cabo, ver v0.21.29): só o cartão
  "Cabo de rede · conectado à internet", "IP na rede local: x.x.x.x" e o texto
  "Com o cabo ligado, o orbita usa só o cabo e o Wi-Fi fica desligado. Para usar o
  Wi-Fi, tire o cabo e reinicie o Xbox." **Sem item de Wi-Fi.**
- Sem cabo: cartão do Wi-Fi (barras, "Wi-Fi: <SSID>", "Conectado · sinal bom (-58 dBm) ·
  IP …" / "Conectando…" / "Desconectado"), linha "Cabo de rede: não conectado" e a opção
  **Wi-Fi** ("procurar, conectar ou esquecer" ›) que abre a `SCR_WIFI` de hoje; B no Wi-Fi
  volta pra Rede.
- Sem adaptador (Fat): "Este Xbox não tem Wi-Fi interno (só 360 S/E). Use o cabo de rede."
- Caso de borda: o cabo pegou DHCP no boot e depois foi tirado. O Wi-Fi não vira rota
  padrão até reiniciar (`WIFI.md`, regra da rota). Mostrar o item Wi-Fi + "Para usar o
  Wi-Fi, reinicie o Xbox sem o cabo."

**Vídeo e Som** (`65_cfg_video_som`; pedido do usuário em 2026-10-27: "Vídeo" vira
"Vídeo e Som", vídeo em cima e som embaixo)
- *Vídeo*: "Resolução da imagem enviada à TV". Opção **720p** "1280 × 720 · 60 Hz" com o
  rádio marcado e "EM USO"; **1080p** "1920 × 1080" apagado com etiqueta Flare
  "EM BREVE" (não recebe foco). Texto: "O 1080p ainda está em teste e chega numa
  próxima versão. A imagem do jogo continua em 720p na nuvem."
- Rodapé do vídeo com a verdade: "Saída atual: HDMI · 720p · 60 Hz" (ou "1080p (teste,
  VIDEO.TXT)" / "Componente" / "VGA", vindo do `xcv`).
- *Som*: "Volume" com quatro opções em rádio: **100%** (padrão), **150%**, **200%**,
  **250%**. Texto: "Se o volume estiver baixo, aumente aqui (por exemplo, fone ligado na
  saída de áudio e vídeo atrás do Xbox 360 S/E)." Vale igual pra HDMI e analógico (a
  diferença só aparece com fone na saída P2 de AV, que é nível de linha).
- A troca aplica na hora (`audio_set_volume_pct`) e grava `XC360/VOLUME.TXT` (o mesmo
  arquivo da v0.21.31, 50..400; valor fora das 4 opções aparece como "personalizado
  (VOLUME.TXT)"). Futuro: trocar os rádios por uma barra.

**Controles** (`66_cfg_controles`, `67_cfg_calibrar_esquerdo`, `68_cfg_calibrar_direito`)
- "Zona morta dos analógicos (vale para todos os controles)". Opções: **Analógico
  esquerdo** "mínimo 37% · máximo 95%" ›; **Analógico direito** "mínimo 27% · máximo 95%" ›;
  **Restaurar padrão** (pede confirmação). Texto: "Zona morta mínima: movimentos
  menores que isso são ignorados (tira o drift). Limite máximo: daqui pra fora o jogo
  recebe o analógico inteiro."
- A em qualquer analógico abre a calibração **começando pelo esquerdo** ("1 de 2"); o
  botão "Próximo: direito" leva ao direito ("2 de 2", botão "Concluir").
- Calibração: círculo de 320 px = curso inteiro do analógico; disco Flare = zona
  morta ("ignorado"); anel Sky = limite máximo ("vai pro jogo"); bolinha branca = posição
  ao vivo com rastro curto; "Posição: 25%" e "Jogo recebe: 0%" (Flare dentro da zona
  morta, Sky fora; no direito, o valor já com a curva da câmera). À direita: sliders
  **Zona morta mínima** e **Limite máximo** (←/→ do **D-pad** ajusta 1%, segurar repete),
  **Medir sozinho**, **Próximo: direito / Concluir**.
- Nesta tela o analógico **não** navega (só o D-pad), senão mexer o analógico pra
  testar andaria o foco.
- Dicas: `D-pad Ajustar · Y Padrão · B Salvar e voltar`.
- Limites: mínimo 0–60%, máximo 60–100%, máximo ≥ mínimo + 10%.
- Medir sozinho: (1) "Solte o analógico" 2 s, guarda o maior raio em repouso `r0`,
  mínimo = `r0 × 1,25 + 2%` (entre 5% e 60%); (2) "Gire o analógico encostando na borda"
  até 5 s, guarda o maior raio em 16 setores; com ≥ 12 setores, máximo = menor desses
  × 0,97 (entre 70% e 100%). Mostra os valores; o usuário ainda ajusta.

### O que tem hoje e para onde vai

| hoje | novo |
|---|---|
| Engrenagem no topo (só com chave) | fica; vale também sem chave (D4); + Guia |
| ID curto/completo, chave mascarada + data, Trocar chave | Conta e sobre |
| Linha da versão | Conta e sobre (bloco VERSÃO) |
| Nota "envie um print" | Conta e sobre |
| Botão Wi-Fi + resumo | Rede (só sem cabo) |
| `X` troca vídeo Auto/720p/1080p (`xcv_settings_cycle`) | sai; Vídeo e Som mostra 720p e 1080p "em breve" (D5) |
| `XC360/VOLUME.TXT` só pelo PC (v0.21.31) | Vídeo e Som: volume 100/150/200/250% |
| Atualizar (topo) | fica no topo; também em Conta e sobre |
| Reiniciar/Desligar (topo) | ficam no topo; também na coluna (D3) |

## 3. Zona morta: como é hoje e o que muda

Hoje (`main.c:359-404`), fixo em compilação:
- esquerdo: `apply_deadzone(…, STICK_DZ=12000)` com `STICK_OUTER=31000` (radial com
  rescala: ≤12000 -> 0, ≥31000 -> 32767);
- direito: `RSTICK_DZ=9000`, mesmo `STICK_OUTER`, e depois `apply_curve` (câmera:
  `0,20 + 0,50 × m^1,5`);
- gatilhos: `TRIGGER_DEADZONE 12` (fica fixo).
- Em %: esquerdo 37%/95%, direito 27%/95% (os padrões da tela).

Mudança:
- `apply_deadzone(x, y, dz, outer)` com os 4 valores vindos da config (padrão = os de
  hoje, então quem não mexer não sente diferença).
- Novo `source/input/xc_padcfg.{h,c}` (C puro, testável no host): padrão, limites,
  parse/format do arquivo, `xpc_apply()` (deadzone + curva, a mesma conta pra jogo e
  pra prévia "Jogo recebe"), e o "medir sozinho".
- Arquivo `XC360/PAD.TXT`: `XCPAD1` / `L <dz> <outer>` / `R <dz> <outer>` (valores
  brutos 0..32767; a tela mostra %). Arquivo ruim/ausente = padrão. Recuperação: apagar
  o arquivo pelo PC, ou `Y` (Padrão) na tela.
- **Threads:** `send_pad` roda na hw1 (rede, 1 kHz). O main publica a config num
  `volatile int32_t g_padcfg[2][2]` (cada palavra é atômica no PPC; mínimo/máximo
  trocados entre duas leituras não fazem mal por 1 ms). Sem lock no caminho do controle.
- **Pendrive:** gravar só com a hw 3 livre, igual `xcv_save_pending`/`catlist`:
  `xpc_save_pending()` nos mesmos pontos (`main.c:2263`, `3148`). Ler no boot junto do
  `VIDEO.TXT`.
- **Por que global (D2):** o slot do libxenon é a ordem de conexão (360 sem fio = índice
  do RF), sem número de série; GIP também não expõe série no nosso parser. "Controle 2"
  hoje pode ser outro amanhã. Dá pra fazer por **tipo** (VID:PID: 360 x Series) depois,
  se pedirem.
- Prévia: a calibração lê **um** controle (o que apertou A pra entrar; se outro mexer
  mais, troca e mostra "Controle N"). O `get_any_pad` mistura eixos de controles
  diferentes, então não serve pra prévia.

## 4. Novidades (notas)

- O manifesto já traz `notes` (≤ 512 B, UTF-8, dentro dos bytes assinados:
  `xc_update.c:166`), e a checagem copia pra `g_st.notes` (`xc_update.c:357`).
- Na instalação OK (`XCU_DONE`, `xc_update.c:~561`, worker na hw 3, que já usa o
  FatFs) gravar `XC360/NOTES.TXT` = `XCN1\n<versão>\n<notas>`.
- No boot ler o arquivo; vale só se `<versão>` = `XCLOUD360_BUILD_VERSION`.
- Sem arquivo, mas a checagem achou `latest == current`: usar `g_us.notes`.
- Data "Instalada em": `mtime` do NOTES.TXT ou a hora do Date gravada junto (`fmt_brt`).

## 5. Arquivos (implementação)

| arquivo | mudança |
|---|---|
| `source/main.c` | `menu_btn_t.guide` (`menu_read_buttons` copia `d.logo`; variante só D-pad pra calibração); Guia conforme a tabela; `SCR_BSET` -> `SCR_CFG` (estado `g_cfg`); preencher os dados da seção; tirar o `X` do vídeo; `send_pad` com `g_padcfg`; `xpc_load` no boot e `xpc_save_pending`; NOTES.TXT no boot; `wifi_close`/Trocar chave voltam pra seção certa |
| `source/settings/xc_cfgnav.{h,c}` (novo) | modelo da navegação (coluna/painel/subpágina, seções visíveis, foco por seção, LB/RB, Guia), puro, igual o `menu_model` |
| `source/input/xc_padcfg.{h,c}` (novo) | config dos analógicos, arquivo, conta, medir sozinho |
| `source/ui/xui.h`, `xui_home.c` | `xui_cfg_t` (seção, lista de linhas `{label, value, flags: seta/apagada/aviso/etiqueta}`, cartão de estado, blocos de info, texto) + `xui_cfg_frame()`; `xui_calib_t` + `xui_calib_frame()` (sujeira: só o quadrado do círculo + leitura quando só a posição mexe); ícones novos (conta, rede, tela) no gerador `tools/gen_xui_assets.py`; o `xui_settings_frame` antigo sai |
| `source/menu_model.{h,c}` | `settings_on` deixa de depender da chave (D4) |
| `source/update/xc_update.c`, `xcu_port*.c` | gravar NOTES.TXT no DONE; expor as notas |
| `source/video/xc_video_xenon.c`, `xc_video.h` | `xcv_settings_state()` (modo atual + origem) pra Tela; `xcv_settings_cycle` sai do uso |
| `source/tcp_stream.c` | `stream_net_cable_ip()` pro cartão do cabo |
| `source/audio/audio_xenon.c`, `audio.h` | já tem `audio_set_volume_pct`/`audio_volume_pct` (v0.21.31); falta gravar o VOLUME.TXT pela tela (pendência no main, igual o VIDEO.TXT) |
| `hosttest/ui/mockup.c` | cenas 60–68 pelo código real (trocam o `settings_v2.c`) + teste de sujeira |
| `hosttest/settings/` (novo) | navegação, Guia, limites/arquivo do PAD.TXT, medir sozinho com sequências gravadas, NOTES.TXT |
| `MENU-UI.md`, `BETA-GATE.md`, `WIFI.md`, `VIDEO-1080P.md` | atualizar os trechos das Configurações |

## 6. Ordem, esforço e riscos

1. Guia abre/fecha as Configurações de hoje (`main.c` só): ~0,5 dia. Dá pra mandar
   sozinho numa beta e confirmar no console com os 3 tipos de controle.
2. Modelo + `xui_cfg_frame` + seções Conta/Rede/Tela + energia na coluna: ~2–3 dias.
3. Zona morta (config, arquivo, `send_pad`, calibração, medir sozinho): ~2 dias.
4. NOTES.TXT: ~0,5 dia.
5. Mockups pelo código real, hosttests, docs: ~1 dia.
Total ~6–7 dias + rodadas de teste no console.

Riscos:
- **Valor ruim de zona morta deixa o jogo injogável** (vale ao vivo, todos os controles):
  limites na tela, Y = padrão, apagar PAD.TXT pelo PC. Pensar em LB+RB no boot = padrão,
  como o 720p forçado.
- **Gravar no pendrive** fora da hw 3 já travou o app (v0.21.19): só pelo `*_save_pending`.
- **Desenho na CPU**: a prévia anima a cada quadro; com sujeira só no círculo
  (~340×400 px) cabe com folga (a rolagem da home já pinta ~510k px).
- **Rede**: "cabo com internet" é decidido no boot (`stream_net_cable`); o caso
  "tirou o cabo depois" precisa do texto de reiniciar.
- **1080p**: tirar o `X` muda o jeito dos testadores testarem 1080p; fica o `VIDEO.TXT`.
- **Guia em controles de terceiros (GIP xpad)**: o patch trata o `0x07` igual pra todos;
  testar ao menos um PowerA/PDP.
