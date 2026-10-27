# Decoder: desempenho, perfil e CPU por thread (v0.21.25)

**Problema:** com 4000 kbps (Wi-Fi sem perda) as cenas pesadas ficam com
`dec_ms` 15-17 e 41-48 fps (~900 quadros pulados); cenas leves, 3-5 ms e 59
fps. O limite é o decode no Xenon.

## O que entrou

Tudo liga/desliga por `XC360/DIAG.TXT` (uma `chave=valor` por linha, junto
das chaves do Wi-Fi). O boot loga `[dec] DIAG.TXT: ...` com o que valeu.

| chave | padrão | o quê |
|---|---|---|
| `decprof=0\|1` | 1 | linha `[dec] prof` a cada 2 s (contadores mftb no libavcodec) |
| `deblock=none\|nonref\|all` | none | quadros que PULAM o loop filter (`skip_loop_filter`) |
| `decthreads=3\|4` | 4 | teto de threads de frame no jogo |
| `spinprio=low\|med` | low | SMT baixa enquanto uma thread espera ociosa no decoder |
| `netprio=low\|med` | low | SMT baixa na hw1 entre os polls da rede |
| `copy=vmx\|memcpy` | vmx | upload Y/U/V pras texturas da GPU |
| `profile=default\|baseline\|main\|high` | default | perfil H.264 na oferta SDP |
| `diag=full\|lite` | full | lite: menos log/estatística no jogo |

### Medição

- `[dec] prof N quadros, T threads (hw3=... sessao=...)`: por quadro, o tempo
  de decode numa thread de frame = entropia (CABAC/CAVLC) + recon
  (predição/MC + IDCT; MC à parte) + deblock + espera por outra thread
  (`ff_thread_await_progress`) + resto (cabeçalhos, refs, fim de linha).
  Também fatias (não-ref, CABAC, IDR). Na 1ª vez: `[dec] stream H.264:
  perfil 100 (High), nivel 4.0, 80x45 MBs, CABAC (oferta: default)`.
  Patch no libavcodec: `~/ffmpeg-xenon-mt-altivec/XCLOUD360-PROF.patch`
  (h264.c, pthread.c) + `libavcodec/xc_prof.h` (novo). Desligado custa uma
  leitura e um desvio por chamada.
- `[cpu] 5s:` a cada 5 s, também no lite: hw0 main % ocupado (fora da espera
  sem AU, da espera da GPU e da espera dentro do decode), hw1 rede % (o
  `g_busy_tb` que já existia) com o custo do áudio (Opus), controle,
  usb+wifi e log (ms/s), e por thread de decoder `hwN trabalho%/espera%`.
- O STAT continua (`prod_ms` = apresentar no main, `cvt_ms` = a cópia
  Y/U/V): antes/depois do `copy=vmx` aparece ali.

### Mudanças que valem sem DIAG

- **Espera ociosa em SMT baixa** (`deps/xenon-pthread`): uma thread do
  decoder esperando outra (ou o main esperando o quadro) girava em
  prioridade média, roubando ciclos do irmão de núcleo que decodifica.
  Agora `or 1,1,1` durante a espera, `or 2,2,2` ao voltar. Também conta o
  tempo ocioso por hw thread (`xpt_idle_tb`).
- **hw1 em SMT baixa entre polls** (`tcp_stream.c`): média só no
  `net_step` e no hook (USB, Wi-Fi, áudio, controle). O main (irmão no
  núcleo 0) apresenta com mais folga. Medir no `[cpu]` e no STAT
  (`a_under`, `ugap_ms`, perda) que não piora.
- **Cópia pra GPU com VMX + dcbz** (`gpu_present.c`): `lvx/stvx` (128 B por
  volta) e `dcbz` nas linhas de cache que o destino cobre inteiras (o
  memcpy lia cada linha do destino antes de escrever). Cai no memcpy se algo
  não estiver alinhado em 16.
- `[dec]`/`[cpu]` usam só o log em RAM: o `xc_log` no main também imprime na
  UART (~87 us por caractere, ~17 ms por linha).

### Perfil H.264 (SDP)

Hoje a oferta só tem o template do app Android, PT 102 com
`profile-level-id=42e01f` (Constrained Baseline 3.1). `profile=main|high`
troca só o profile-level-id desse PT (4d001f / 64001f): o libpeer só aceita
vídeo no PT 102, então não dá pra oferecer vários PTs como o Better xCloud
faz no navegador. O log mostra a linha `a=fmtp` oferecida e o `m=video`,
`a=rtpmap`, `a=fmtp` da resposta. Se o servidor recusar o vídeo
(`m=video 0`), a próxima oferta (a 2ª tentativa da sessão) volta pro padrão.
O perfil que chega de verdade vem do SPS (`[dec] stream H.264: perfil ...`).
Se já vier Baseline/CAVLC com o padrão, `profile=baseline` não muda nada.

### diag=lite

- NETLOG (escrita direta no pendrive na sessão) desligado, a menos que o
  DIAG.TXT tenha `netlog=` explícito;
- estatísticas `[wifi] firmware`/HS/sessão/locks a cada 30 s no jogo (eram 5
  s); fora do jogo, como antes;
- STAT sem os campos da GPU e do `ugap`;
- logs do libpeer/usrsctp (`[dc]`, `[peer]`...) no jogo só se tiverem
  erro/aviso.
- Ficam: vigia/tripwires, resumos, erros, `[dec] prof` (se `decprof=1`) e
  `[cpu]`. O `[cpu]` mostra o `log ms/s` do main e da hw1 pra comparar
  full x lite.

### O que não entrou

- **`decthreads=5` na hw0**: não dá com o libxenon. Cada hw thread roda uma
  tarefa só (`xenon_run_thread_task`), sem escalonador; a hw0 é o próprio
  laço do main. O frame threading do ffmpeg 0.10 põe cada worker numa
  thread própria (o `xenon-pthread` mapeia uma pthread por hw thread livre)
  e quem chama o `avcodec_decode_video2` só entrega o pacote e espera: não
  decodifica junto. Pra usar a hw0 seria preciso o main decodificar
  pedaços entre os flips (slice threading, que o stream do xCloud não usa
  de forma útil, ou um decoder próprio por fatias). O ganho fácil na hw0
  agora é a espera dele não roubar a hw1 (SMT baixa) e a cópia mais rápida.
- **Flags de compilador**: o libavcodec já sai com `-O3 -mcpu=cell
  -mtune=cell -fomit-frame-pointer` (`-fno-tree-vectorize` é obrigatório,
  ver README-XCLOUD360.md). Não dá pra medir variações sem o console;
  fica pra depois do perfil mostrar onde o tempo vai.
- **Decoder escrevendo direto na textura (zero cópia)**: com frame threads
  os quadros do decoder são referência de quadros futuros e mudam de dono;
  entregar o buffer pra GPU exige contar referência até o flip terminar
  (get_buffer próprio + release no fence). Fica pra v0.21.26 se o
  `cvt_ms` com `copy=vmx` ainda pesar.
- `CODEC_FLAG2_FAST` já estava ligado.

## v0.21.26

### Threads do decoder (por que eram 3 e a hw4 ficava parada)

- O `usrsctp_init_nothreads` (chamado no `xn_init`, no boot) ainda cria a
  thread do *iterator* do SCTP (`sctp_startup_iterator`). No `xenon-pthread`
  cada pthread ocupa uma hw thread inteira, e a primeira livre da lista era
  a **hw4**: o iterator ficava lá pra sempre, esperando trabalho que nunca
  vem (o `[cpu]` mostrava `hw4 0%/0%`: ninguém decodificando, e a espera dele
  nunca terminava pra ser contada).
- Com a hw4 ocupada, o decoder pegava 2,5,**3** sem alugar a hw3. Isso fazia
  o `xst_running()` dar "thread da sessão viva" pra sempre, o
  `xn_dec_threads_wanted` nunca subia pra 4, e se a sessão precisasse da
  hw3 de volta ela ficaria esperando.
- Correção:
  - `xpt_park_new` (deps/xenon-pthread): o main liga em volta do `xn_init`;
    pthreads criadas nessa janela ficam *estacionadas* (sucesso pro
    usrsctp, nunca rodam, `pthread_join` volta na hora). O iterator só
    serve pra varrer associações em mudança de endereço/ASCONF, que não
    usamos (uma associação, sem ASCONF). Log:
    `[dec] pthreads estacionadas no xn_init: 1 (iterator do usrsctp)`.
  - `xdd_open_pinned`: as threads de frame só podem ficar nas hw 4,2,5 (e a
    3 só com o aluguel `xn_dec_claim_hw3` na mão). Se faltar hw, tenta com
    menos threads. Log: `[dec] abriu 3 threads de frame (pedido 3) nas hw
    4,2,5; permitidas 4,2,5; ocupadas antes: hw1 hw3; estacionadas 1`.
  - Fluxo esperado: começa com 3 (hw 4,2,5) enquanto a thread da sessão
    está na hw3; quando ela sai (~18 s) e a sessão está no hw1 há 3 s, o
    decoder reabre com 4 (hw 4,2,5,3), custo de 1 IDR:
    `[dec] jogo nativo: threads 3 -> 4 (hw3 de volta)` e `[dec] abriu 4
    threads ... nas hw 4,2,5,3`.

### MC em AltiVec pra partições 8x8

- O libavcodec 0.10.2 só tem MC luma em AltiVec pra 16x16 (já ligado). 8x8,
  8x16 e 16x8 (boa parte de um stream Main de jogo) rodavam em C.
- Novo `libavcodec/ppc/h264_qpel8_xenon.h` (incluído pelo h264_altivec.c,
  só com `XCLOUD360_VMX_SAFE`): put/avg das 16 posições 8x8, com h/v
  em s16 (8 pixels = 1 vetor) e o hv na 2a etapa em s32, só com instruções
  que o Xenon tem. Patch: `~/ffmpeg-xenon-mt-altivec/XCLOUD360-QPEL8.patch`.
- Verificado no qemu (harness do track A, `~/scratch/wf/trackA`, build
  `qb-av8`): 4 streams (Main CABAC bf0/bf3 e os do xCloud), 1 e 3 threads,
  **todos os quadros iguais à referência x86**. Teste de mutação (quebrar o
  arredondamento h/v, o 512 do hv, o avg e o l2) muda a saída já no quadro
  1: os caminhos novos rodam de verdade.
- O que continua em C: qpel 4x4 (partições 4x4/8x4/4x8), croma mc4/mc2,
  deblock de croma e o cálculo de força do deblock (o luma já é AltiVec),
  intra.

### Observações dos logs da v0.21.25

- O servidor manda **Main (77), nível 3.2, CABAC** mesmo com a oferta 42e01f:
  `profile=baseline` não muda nada. A opção fica.
- Por quadro (cena leve): decode 15,9 ms = entropia 2,3 + recon 8,9 (MC 7,5)
  + deblock 3,2 + resto 1,5. Picos: 37-43 ms. Atenção: o MC inclui a espera
  por referência (`await_references` fica dentro do `hl_motion`), então nos
  picos "MC" e "espera" se sobrepõem (por isso o "resto" fica negativo).
- `cvt_ms` 4,5 -> 1,4 ms com `copy=vmx`.
- Janela de ~20 fps com `dec_ms` 1,3 e `ring=0` (STAT t=66-68): `kbps=8`,
  perda 0, NACK 0, Wi-Fi normal (RX contínuo, buraco máx 26-28 ms, sem
  descartes), áudio sem underrun. O servidor mandou quase nada de vídeo
  (cena parada: menu/pausa/carregamento; o encoder do xCloud baixa a taxa
  de quadros com imagem estática). Não é rede nem decoder.

### Ganho esperado (estimativa)

- 4 threads de verdade (hw2-5, a hw4 que estava parada): +25-35% de vazão
  sobre o que rodava (3 threads, duas delas dividindo o núcleo 1).
- qpel 8x8 em AltiVec: o 16x16 em AltiVec costuma ser 3-4x mais rápido que
  o C; nas partições 8x8 o ganho deve ser parecido no cálculo, mas o MC
  aqui também espera memória (blocos de referência), então algo como 20-40%
  a menos no "MC" do prof.

## Ordem de teste sugerida (2500-4000 kbps, a mesma cena pesada)

1. Sem DIAG.TXT novo (tudo padrão): olhar `[dec] prof` (onde o tempo vai,
   quantas threads, `hw3=`), `[dec] stream H.264` (perfil/CABAC) e `[cpu]`.
2. Se `[dec] prof` mostrar 3 threads com `hw3=livre sessao=hw1`, o 4o
   worker não entrou: mandar o log.
3. `deblock=all`: mede o quanto o deblock pesa (qualidade: sem filtro, os
   quadros de referência derivam do encoder até o próximo IDR; blocos
   visíveis em movimento). `deblock=nonref` só ajuda se o `[dec] prof`
   mostrar fatias não-ref (stream do xCloud costuma ser todo referência).
4. `profile=main` e `profile=high` (uma sessão cada): ver o que o servidor
   responde e se o `[dec] stream` muda.
5. `spinprio=med`, `netprio=med`, `copy=memcpy`: só pra confirmar o ganho
   de cada um contra o padrão.

## Ganho esperado (estimativa, sem medição no console)

- 4a thread (hw3) com a sessão fora: +10-25% de vazão (divide o núcleo 1
  com a hw2 por SMT). Latência: frame threading entrega o quadro
  `threads-1` quadros depois, então 4 threads = +1 quadro (16,7 ms) sobre 3.
- SMT baixa na espera: as threads de decoder esperam bastante umas pelas
  outras (o `espera` do prof mostra); o irmão de núcleo ganha esses ciclos.
  Estimativa 5-15%.
- `deblock=all`: 15-30% do decode em H.264 de bitrate alto.
- `copy=vmx`: `cvt_ms` ~4,5 -> ~1,5-2,5 ms no main.
- CAVLC (baseline) no lugar de CABAC: a entropia costuma ser 30-50% do
  decode em bitrate alto; CAVLC é mais barato, mas o servidor decide.
