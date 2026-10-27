# HDMI 1080p (escalado de 1280x720)

**Por quê:** a TV de um testador mostra "resolução não suportada" com o HDMI
720p do libxenon. O libxenon não tem modo 1080p, e não existe tabela ANA
1080p pública (procurei no libxenon, nos patches Free60 linux-kernel-xbox360
e no GitHub). A `ana_hdmi_720p_corona` do libxenon veio de um dump de console
retail; o 1080p segue o mesmo caminho: dump com o XEX local `orbita-dump`.

**Estado (26/10):** tabela **Corona/Winchester** gerada do dump do Slim do
dev (kernel 2.0.17559, DVE 0x23); pre-Corona continua placeholder (fica em
720p). Build de teste `variants/v0212-video.elf32` (0.21.2-video), ainda não
testada no console.

### Validação do método (dump 720p x `ana_hdmi_720p_corona`)

- Família da placa = DVE (ANA 0xFE) >= 0x20, igual ao `isCoronaOrWinchester`
  do libxenon e ao app. O `XboxHardwareInfo` flags>>28 = 5 segue a numeração
  0-based do libxenon (5 = Corona), não "Trinity"; ele não decide nada.
- Os 72 índices ANA que o libxenon escreve: **72/72 iguais**. CRTC lido =
  1650x750/259/25, igual ao libxenon/CEA.
- Os 6 outros índices que diferem não são escritos pelo libxenon a partir da
  tabela: `0x7f` (muda entre 720p/1080p e entre consoles: contador/estado),
  `0xeb-0xed` (medidas ao vivo da DAC/PLL, mudam até entre a fase A e B do
  mesmo dump), `0xf0/0xf1` (o dump tem 0x0291028e/0x028e0285, exatamente as
  constantes que o `xenos_init_ana_new` escreve na Corona; a tabela do libxenon
  guardou outro valor e ele nunca é usado).
- 720p -> 1080p no dump muda só: `0x12-0x15` (timing HDMI da HANA: 0x13 low =
  0x898 = 2200, 0x15 low = 0x465 = 1125; no 720p 0x672/0x2ee), `0xd0/0xd1`
  (PLL: 0xd1 0x546 -> 0xa8c, o dobro do pixel clock) e os registradores ao
  vivo acima. `0xd2` igual. O dashboard estava em 59,94 Hz (como o 720p do
  libxenon), então o 1080p sai a 59,94 Hz (VIC 16 aceita).
- CRTC 1080p lido (0x6000=0x897, 0x6004=0x00bf083f, 0x6010=0x464,
  0x6014=0x00290461) = exatamente o que o `xenos_set_mode_f1` escreve com o
  timing CEA da tabela.
- Scaler (fase B, back buffer 1280x720 -> 1920x1080): viewport 0x6584 =
  0x050002d0, enable 0x6590 = 1, TAP 0x6594 = 0x504 (0x904 em 720p), razões
  **0x65b4 e 0x65c4 = 0x00aaaaa0** (2/3 com os 5 bits de baixo zerados),
  fases iniciais 0x65b8 = 0x3d600 / 0x65c8 = 0x35600, 0x652c (desktop height)
  = 0x438. Esse bloco vai inteiro no POST da tabela. Fora dele, entre 720p e
  1080p só mudam 0x6054-0x6068 e 0x6554 (estado/posição do CRTC; o libxenon
  escreve constantes ali em todos os modos) e a superfície do D3D (D1GRPH,
  que o app não copia).

## v0.21.22: causa achada, filtro do scaler

**Prova:** um frame da placa de captura (v0.21.19) mostra o menu inteiro como
**detecção de borda**: áreas lisas pretas, só os contornos, no tamanho e
posição certos em 1920x1080. Timing, ANA, matriz de cor e LUT estão certos;
o filtro do scaler tem ganho DC ~0 (passa-alta).

**Por quê:** o D1SCL do Xenos tem o layout do DCE da AMD (headers públicos
`dce_6_0_*`/`dce_8_0_*` do Linux), com 0x6590 = SCL_SCALER_ENABLE e +4 por
registrador: 0x6594 TAP_CONTROL (V taps-1 em [2:0], H taps-1 em [11:8]),
0x65b0/0x65b4/0x65b8/0x65bc = filtro H (controle, razão, fase RGB, fase
croma), 0x65c0/0x65c4/0x65c8 = filtro V, 0x65cc = UPDATE. Confirmado pelos
dumps: a fórmula do DCE para a fase inicial, `(razão + taps + 1) / 2`
arredondada a 0x200, reproduz os **8** valores que o kernel escreveu
(720p/1080p, fases A/B, 10/6/5 taps). O kernel usa 6x5 taps, que leem a **RAM
de coeficientes** carregada pelo kernel; o libxenon nunca liga o scaler, então
a RAM fica zerada/lixo: o resultado é a imagem de contornos.

**Correção:** em vez de carregar a RAM de coeficientes (as portas
SELECT/TAP_DATA do DCE ficariam em 0x6588/0x658c, que no AVIVO R5xx são o
overscan; endereço incerto no Xenos, então não escrevemos às cegas), o
scaler usa **2 taps com coeficientes bilineares fixos do hardware**:
FILTER_CONTROL bit 8 = 2TAP_HARDCODE_COEF_EN (DCE8; o radeonhd liga o mesmo
bit no R5xx com 0x00030100). TAP_CONTROL = 0x101, 0x65b0 = 0x100,
0x65c0 = 0x01000100 (o valor do kernel + bit 8), fases recalculadas para 2
taps (0x1d600 pra 2/3). Bilinear: pesos (1-φ, φ) somam 1, nenhum negativo
(sem ringing). O modelo no host (`test_scl_model`) confere liso -> liso,
degrau monotônico sem overshoot, e que o modelo passa-alta (o que vimos) zera
o liso.

| VIDEO.TXT | filtro |
|---|---|
| `1080` | 2 taps bilinear fixo (padrão) |
| `1080r` | 2 taps, controles exatos do radeonhd (0x00030100) |
| `1080e` | ponto mais próximo (1 tap, PICK_NEAREST): serrilhado, mas não depende de coeficiente nenhum |
| `1080k` | 6x5 taps do kernel (reproduz a imagem de contornos, pra comparar) |
| `1080n` | scaler desligado (1280x720 no canto) |

`c`/`x` continuam (CRTC MVP/TRIG e D1MODE extras). Log novo:
`[video] scaler: 2 taps bilinear fixo, taps 2x2, 6594=... 65b0=... 65b8=...
65c0=... 65c8=...` além das linhas `SCL`/`agora SCL`.

Se o bilinear fixo ainda vier errado: testar `1080e` (sem coeficiente
nenhum); se esse ficar certo, falta só achar as portas da RAM de
coeficientes (próximo dump: ler 0x6588/0x658c/0x6598-0x65ac depois de
escrever um índice, num XEX com o kernel ativo).

## v0.21.21: 1080p "quase preto" + travamento do X

**Primeiro teste (v0.21.19, placa de captura HJW, VIDEO.TXT=1080p):** a
captura aceitou o modo, mas a imagem saiu quase toda preta. O que o dump mostra:

- Cor e níveis **não** mudam com o modo: a matriz de cor do kernel em 1080p
  (0x6380-0x63b0) é idêntica à do libxenon `rgb=1`, e a LUT é a mesma do 720p
  que funciona (o `xenos_set_mode` refaz o autofill nas duas). O CRTC (timing,
  0x6008/0x6018 sync, 0x600c/0x601c) e os clocks da GPU são idênticos ao
  kernel. A ANA só muda timing HDMI (0x12-0x15) e PLL (0xd0/0xd1).
- O que o caminho da v0.21.19 deixava **diferente do kernel em 1080p**:
  - D1CRTC 0x6038-0x6068 (MVP/TRIG; o libxenon chama de BLACK_KEYER/TRIG):
    o kernel muda com o modo (720p 0x6058=0x962ff00f, 1080p 0x6b36401a,
    trigger 0x96 -> 0x6b...); o libxenon escreve constantes de 720p.
  - D1MODE: 0x6524 (libxenon 0x300, kernel 0; vizinho do
    DC_LB_MEMORY_SPLIT 0x6520 = line buffer, que o scaler vertical de 4 taps
    usa), 0x65e0 (kernel 1, libxenon não escreve), 0x6578 (kernel 0x30000).
  - O scaler: no 720p do libxenon ele fica **desligado**; em 1080p é a
    primeira vez que o app liga o scaler. O kernel deixa o scaler ligado até
    em 720p (razão 1.0).
- Não dá pra ter certeza sem ver a imagem, então a v0.21.21 é "o mais igual
  ao kernel possível" + letras de diagnóstico no `XC360/VIDEO.TXT`:

| VIDEO.TXT | o quê |
|---|---|
| `1080` | padrão novo: POST do scaler + D1MODE extras + MVP/TRIG do CRTC, tudo do dump |
| `1080n` | scaler desligado: a imagem 1280x720 no canto de cima/esquerda do quadro 1080p. Se aparecer normal, a ANA/timing está certa e o problema é o scaler |
| `1080c` | MVP/TRIG com as constantes do libxenon |
| `1080x` | sem os D1MODE extras (0x6520/0x6524/0x6578/0x65e0) |
| `1080cx` | exatamente o caminho da v0.21.19 |

  Ordem sugerida: `1080` e, se ainda escuro, `1080n`. Um print da placa de
  captura (OBS) ajuda mais que descrição.
- Log novo (boot, 720p **e** 1080p, pra comparar): linhas `[video] CRTC`,
  `CRTC2`, `GRPH`, `MODE`, `SCL`, `CSC`, `LUTREG`, `LUT` (entradas 0/1/64/
  128/192/255 lidas pela porta da LUT), `ANA` (0x00, 0x12-0x15, 0xcd,
  0xd0-0xd2, 0xd6, 0xd8, 0xdf); no menu ainda `agora GRPH/SCL/MODE` (relidos
  depois do `gpu_present_init`, pra ver se alguém mexeu na superfície/scaler).

**Travamento do X nas Configurações:** o `xcv_settings_cycle` gravava o
`VIDEO.TXT` direto do main (FatFs/USB com o worker das capas ou a hw1). Agora
o X só muda a memória e marca pendente; quem grava é `xcv_save_pending()`,
chamado nos mesmos dois lugares do `catlist_save_pending()` (main com a hw 3
livre e `worker_extra_step`). O X sempre grava o modo sem letras.

## Como funciona

- O app continua desenhando num **framebuffer 1280x720** (UI, `gpu_present.c`,
  orçamento de EDRAM, `fb_direct_init`: tudo lê D1GRPH = 1280x720). Em 1080p,
  só a saída muda: CRTC em 1920x1080 (CEA 1080p60, 148,5 MHz, total 2200x1125)
  e o scaler AVIVO do D1 aumenta 1280x720 -> 1920x1080.
- `main()` sobe em 720p como antes (`orbita_video_mode`, pelo AV pack). Depois
  do `usb_init` e do warm-up (pendrive e controles prontos) e **antes** do
  `fb_direct_init`/`gpu_present_init`, `xcv_boot(g_avpack)` decide:
  - componente / VGA: 720p como antes (EDID e escolha manual não contam);
  - HDMI com **LB+RB segurados** no boot: 720p forçado (escape);
  - `XC360/VIDEO.TXT` = `720p` ou `1080p`: manual;
  - `auto` (ou sem arquivo): lê o EDID (`xenos_get_edid`). Se a TV **não**
    lista 1280x720@60 (VIC 4/69, DTD ou standard timing) mas lista
    1920x1080@60 (VIC 16/76, DTD ou standard timing): 1080p. Senão 720p.
    Sem EDID: 720p.
  - 1080p (manual ou auto) só vale se existir tabela validada pra placa
    (`xcv_1080p_usable`: pre-Corona e Corona/Winchester têm tabelas separadas,
    como no libxenon). Sem tabela: 720p.
- 1080p = o mesmo `xenos_set_mode()` do libxenon (ANA pelo SMC + CRTC +
  D1GRPH) com uma `struct mode_s` nossa (CRTC 1920x1080, superfície/viewport
  1280x720), depois o bloco D1MODE/D1SCL da tabela entre `D1SCL_UPDATE=1/0`.
  Sem fork do libxenon: `xenos_set_mode`, `xenos_write32` e `xenos_is_corona`
  já são públicos (só não estão no header). A cópia da `struct mode_s` fica
  em `xc_video_xenon.c` e precisa bater com `xenos_videomodesdata.h`.
- Na TV: o boot mostra 720p por ~3 s (até o `xcv_boot`) e então troca. Numa
  TV que não aceita 720p isso é só tela preta/aviso até a troca.
- **Configurações** (engrenagem): a linha da versão mostra
  `vídeo: Automático (agora 720p) (X troca)`. X troca Auto -> 720p -> 1080p
  e grava `XC360/VIDEO.TXT`; vale no próximo boot. (Sem mexer no layout de
  `source/ui`, que o Wi-Fi está mudando; dá pra virar uma linha própria depois
  com `xcv_settings_line()`.)
- Se o 1080p escolhido à mão der tela preta: segurar **LB+RB** ao ligar (720p
  nesse boot), ou apagar `XC360/VIDEO.TXT` do pendrive no PC.
- Log (`[video]`, UART no boot e repetido pro upload no `menu_main`):
  `AV pack 0x1f (HDMI), placa Corona/Winchester, tabela 1080p: ...`,
  `EDID: SAM 'T24D390' hdmi v1.3 ext=1 cea=1 pref 1920x1080p60.00 720p60=0
  1080p60=1 1080i60=0 VIC 16*,31` e `VIDEO.TXT=auto -> HDMI 1080p (...)
  (auto: EDID sem 720p60 e com 1080p60)`. Número de série do EDID nunca vai
  pro log.

## Arquivos

| arquivo | o quê |
|---|---|
| `source/video/xc_edid.[ch]` | EDID base + extensões CEA (VICs, DTDs, standard timings, VSDB HDMI). Puro C. |
| `source/video/xc_video_mode.[ch]` | escolha do modo, `VIDEO.TXT`, timing CEA -> campos da `mode_s`, razão do scaler, checagem da tabela. Puro C. |
| `source/video/xc_video_1080p_data.h` | **a tabela** (gerada). Único arquivo que muda quando chega um dump. |
| `source/video/xc_video_xenon.c`, `xc_video.h` | Xbox: lê VIDEO.TXT/EDID/LB+RB, aplica o 1080p, log, Configurações. |
| `tools/video/dump2mode.py` | dump -> `xc_video_1080p_data.h` (`gen`), e validação do método contra o 720p do libxenon (`check720`). |
| `hosttest/video/` | `run.sh` (entra no `hosttest/run.sh`), EDIDs reais em `edid/*.hex`, `fake_dump.py` (dump sintético). |
| `main.c` | 5 ganchos de uma linha: include, `xcv_boot`, `xcv_log_summary`, X nas Configurações, linha da versão. |

## O dump (XEX local, fora do git)

Fonte e build em `C:\build-xdk\orbita-dump\` (Windows, XDK 2.0.21256,
`build.cmd`): **nunca** entra no repo (headers/libs/binários do XDK). O XEX
vai pra `Desktop\windows desktop compartilhado\orbita-dump\` com um
`LEIA-ME.txt` (dashboard em 1080p e depois 720p, abrir pelo Aurora, tela
verde = gravou `orbita-dump-1080p.txt` / `-720p.txt` na pasta do XEX).

O que ele lê, **só leitura**:
- **ANA/HANA 0x00..0xFF**: mensagem SMC `11 10 05 F0 00 F0 <idx>` (a mesma
  do `xenon_smc_ana_read` do libxenon), pelo export do kernel
  `HalSendSMCMessage` (xboxkrnl #41, que serializa com o resto do kernel). O
  SMC só devolve o valor; nada é escrito. Índices que o SMC recusa saem `ERR`
  (viram `0xffffffff`, como nas tabelas do libxenon).
- **Display controller da GPU**: loads de 32 bits na janela de registradores
  que o D3D do título usa (virtual `0x7FC80000` = físico `0xEC800000`, o mesmo
  bloco do `xenos_read32` do libxenon), dentro de `__try/__except`: CRTC
  `0x6000-0x60FC`, GRPH `0x6100-0x617C`, matriz de cor `0x6380-0x63BC`, LUT
  (pulando as portas de dados `0x648C-0x6498`, cuja leitura avança o índice
  da LUT), D1MODE/D1SCL `0x6500-0x65FC`, clocks e `0x7900/0x7910/0x7938/0x793C`.
  Não lê `0x7950/0x7954` (porta indireta da ANA). Ler esses registradores
  AVIVO não tem efeito colateral.
- **Placa**: `XboxHardwareInfo` (#342, tipo = flags>>28), ANA `0xFE` (versão
  DVE, o critério Corona do libxenon), AV pack (SMC `0x0F`), `XGetVideoMode`,
  versão do kernel.
- Fase **A** = estado herdado (antes do D3D); fase **B** = depois de criar um
  device D3D com back buffer **1280x720** e apresentar ~0,5 s: é o scaler que
  o kernel programa pra 1280x720 -> TV, exatamente o que o app quer.
- O arquivo é regravado inteiro depois de cada etapa (info, ANA, GPU), então
  um travamento no meio ainda deixa o que já foi lido.

## Quando o dump chegar

```sh
cd app/h264play
# 1) valida o método: o dump 720p tem que bater com a ana_hdmi_720p(_corona)
python3 tools/video/dump2mode.py check720 orbita-dump-720p.txt
#    (índices que o libxenon usa: 100% iguais; CRTC = 1650x750/259/25)
# 2) gera a tabela (um dump 1080p por tipo de placa; --ref720 do MESMO console)
python3 tools/video/dump2mode.py gen orbita-dump-1080p.txt --ref720 orbita-dump-720p.txt
# 3) revisar o diff de source/video/xc_video_1080p_data.h (ANA, CRTC, POST)
hosttest/video/run.sh
make && python3 ~/ffmpeg-xenon-mt-altivec/xcloud360/vmxaudit.py h264play.elf
```

O que revisar no `gen`:
- `CRTC do dump != CEA`: o kernel pode programar o CRTC com outra
  convenção; o gen usa o do dump se der 1920x1080 ativo, senão o CEA.
- O `POST` (D1MODE/D1SCL da fase B): deve ter viewport `0x050002d0`
  (1280x720), scaler ligado e razões ~`0x00AAAAAB`. O gen confere a ordem de
  bytes pela `D1GRPH_X_END`.
- Placa do dump: a tabela só serve pro tipo de placa dela (Corona/Winchester
  ou pre-Corona). Um testador com outra placa continua em 720p até ter um
  dump dessa família.
- Depois, teste no console: TV que aceita 720p com `VIDEO.TXT=1080p`, ver
  imagem cheia e nítida (scaler), `[video]` no log, LB+RB volta pra 720p.

## Pontos em aberto

- `xcv_scaler_regs` (reserva, só pra tabela sem POST) agora usa os offsets
  do dump (0x65b4/0x65c4); o teste confere que bate com o POST real.
- pre-Corona: precisa de um dump 1080p de um Xenon/Falcon/Jasper/Trinity.
- `D1CRTC_H_SYNC_A` (0x6008) etc. ficam nos valores fixos do libxenon (iguais
  em todos os modos); no HDMI quem gera o timing final é a HANA (ANA
  0x0C-0x17), que vem do dump. Se a TV reclamar, comparar a fase B do dump com
  o que o `xenos_set_mode_f1` escreve.
- Se a TV do testador rejeitar o 720p mesmo listando 720p60 no EDID (ex.:
  algum detalhe do sinal 720p do libxenon), o auto fica em 720p: aí a saída é
  o manual 1080p (X nas Configurações, ou `VIDEO.TXT` pelo PC).
- EDIDs de teste: 5 da base pública linuxhw/EDID (caminhos no cabeçalho de
  cada `.hex`) e 2 do dev; número de série zerado em todos.
