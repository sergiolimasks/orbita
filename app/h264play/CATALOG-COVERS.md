# Menu de capas: camada de dados (source/catalog)

Dados para o menu em fileiras, no estilo do xbox.com/play: fileiras, capas e
hero. O desenho fica a cargo do menu_ui. Este módulo não tem código de UI.
Status (v0.20.0): integrado ao `main.c` (veja "Integração"). v0.20.0: hero em 1280x720, nomes em UTF-8, caches versionados (abaixo).

## Arquivos

| arquivo | o que é |
|---|---|
| `source/catalog/xc_catalog.h` | API pública (main, ganchos do login, worker, partes puras) |
| `source/catalog/xc_catalog.c` | estado, fileiras, índice de imagens, fila, LRU, worker |
| `source/catalog/xcc_parse.c` | parsers puros (SIGL, displaycatalog), URL de tamanho, nome 8.3 |
| `source/catalog/xcc_image.c` | JPEG → pixels (stb_image só JPEG, C puro) + escala/corte |
| `source/catalog/xcc_net.c` | GET HTTPS com uma conexão keep-alive por host (https_conn_*) |
| `source/catalog/xcc_port.h`, `xcc_port_xenon.c` | lock/lwsync, hw thread 3, pilha de 256 KB |
| `source/catalog/stb_image.h` | stb_image v2.30 (domínio público/MIT, veja THIRD_PARTY.md) |

Mudanças pequenas fora da pasta, todas sem efeito enquanto o main não chamar `xcc_init`:
- `login_flow.{c,h}`: `lf_set_hooks()` com `on_start` / `on_cloud` / `on_titles`.
- `xbox_auth.{c,h}`: `xa_fetch_mru()`, que reaproveita o GET do `/v2/titles`.
- `login_task.c` + `xc_port.h`: `xcn_set_cancel_hook()`. O `xcn_cancelled()` passa a ver também o pedido de parada do worker.
- `xc_port_xenon.c` (e `hosttest/login/port_posix.c`): o `xc_store_save("COVERS/X.JPG")` cria a subpasta 8.3.
- `Makefile`: `SOURCES += source/catalog` (dentro do `XBOX_LOGIN=1`).

## API (main, hw 0: nada bloqueia nem toca rede/USB)

```c
xcc_init(NULL);                       // boot, ANTES do lt_start (registra os ganchos do login)
xcc_poll();                           // 1x por quadro: capas decodificadas entram no LRU
if (xcc_rows_copy(&rows)) redraw();   // static xcc_rows_t rows; 1 = mudou
// rows.row[XCC_ROW_RECENT|POPULAR|NEW|ALL]: .title, .n, .items[i].{id,product,name,flags}
xcc_want(XCC_COVER, visiveis, n);     // bigIds na tela, em ordem de importância
xcc_want(XCC_HERO, &selecionado, 1);
const xcc_image_t *c = xcc_cover_get(product);  // NULL = ainda não (e entra na fila)
const xcc_image_t *h = xcc_hero_get(product);
xcc_mru_touch(title_id);              // depois de um PLAY: sobe em "Voltar a jogar"
xcc_bg_start(); xcc_bg_stop(); xcc_bg_running();
xcc_seq();                            // muda quando fileiras ou capas mudam
xcc_status(&st);                      // contadores pra overlay/log
```

- Fileiras: `XCC_ROW_RECENT` "Voltar a jogar" (até 40), `XCC_ROW_POPULAR` "Mais populares na nuvem" (até 40), `XCC_ROW_NEW` "Adicionados recentemente" (até 40), `XCC_ROW_ALL` "Todos os jogos" (até 1024, alfabético, é a lista do login). Todas aparecem filtradas pelo catálogo jogável da conta. Os títulos são ASCII e cabem na fonte 8x16.
- `xcc_image_t`: `{w, h, px}`. `px` traz `w*h` pixels no **mesmo formato do `UI_RGB`** do menu_ui (`(b<<24)|(g<<16)|(r<<8)`), linha a linha, pronto pro `pset`.
  O ponteiro vale até o próximo `xcc_poll()`, porque é ali que o LRU despeja.
- Capa: 176x264 (retrato 2:3). Hero: **1280x720** (16:9, v0.20.0; era 640x360 ampliado 2x no menu).
- RAM: LRU de 64 capas (~186 KB cada, ~12 MB) e **2** heroes (~3,7 MB cada, v0.20.0). Ajuste via `xcc_config_t`.
  Pedidos visíveis acima da capacidade do LRU são ignorados, pra não decodificar e despejar em laço.

## Threads

- **worker** (hw 3, `XCC_HW_THREAD`): faz rede (HTTPS sobre o net_pipe, como o login), pendrive (`xc_store_*`, que já faz o `xc_usb_hold`) e decode.
  - Só sobe por `xcc_bg_start()`. Retorna -1 se a hw 3 estiver ocupada (login ou sessão).
  - Ocioso, espera com `xcn_sleep_ms` (= `xc_polite_udelay`).
  - `xcc_bg_stop()` não bloqueia: marca a parada, e o gancho de cancelamento derruba o pedido HTTPS em voo (no host: 7–31 ms). Depois o worker grava o índice e fecha as conexões.
- **login** (hw 3, login_task): ganchos `lf_hooks_t`.
  - `on_start`: carrega o cache do pendrive. O menu já mostra as fileiras enquanto o TLS do login roda.
  - `on_cloud`: `GET <região>/v2/titles/mru?mr=25` com o gsToken (o token não é guardado nem logado).
  - `on_titles`: recebe a lista final, refaz as fileiras e grava `ROWS.TXT`. Custo: 1 pedido HTTPS a mais no login, mais uma gravação de ~50 KB no pendrive.
- **main**: só lock (spin `lock()` do libxenon) + cópia. Os pixels são alocados pelo worker e liberados pelo main, no mesmo padrão do `lt_take_games`.

Ordem de trabalho do worker em cada `xcc_worker_step()`:
1. cache;
2. `ROWS.TXT` pendente;
3. SIGLs (uma vez por sessão; se falhar, tenta de novo em 60 s);
4. **pedidos visíveis**: lote do displaycatalog se faltar a URL, depois pendrive ou download, e decode;
5. fundo: URLs e capas que faltam, na ordem das fileiras, **só pro pendrive** (sem decode), com 250 ms entre downloads;
6. `IMGIDX.TXT` (a cada 24 mudanças, ou quando o fundo acaba).

Tratamento de falhas:
- Falha por imagem: backoff de 4 s até 4 min, desistindo depois de 5 tentativas.
- 3 erros de rede seguidos: pausa de 15 s.
- Sem rede: ainda serve o que já está no pendrive.

## Endpoints (todos confirmados do devbox em 2026-10-25, exceto o MRU)

| o quê | pedido | notas |
|---|---|---|
| Populares | `GET https://catalog.gamepass.com/sigls/v3?id=6a589fa0-d493-472b-8e20-3813699d7056&market=BR&language=pt-BR&subscriptionContext=none&platformContext=Cloud%3AXGPUWEB` | público, sem token. ~3628 ids em ordem de popularidade (80 KB) |
| Adicionados recentemente | mesmo pedido com `id=06323672-b8c8-43cc-b0de-32d5a9834749` | 24 ids. Se a v3 falhar, a v2 (`/sigls/v2?id=&market=&language=`) é o fallback |
| Imagens | `GET https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=A,B,..(20)&market=BR&languages=pt-BR&fieldsTemplate=Browse` | `LocalizedProperties[0].Images[]`: `ImagePurpose` + `Uri` (`//store-images...`) |
| Capa / hero | `GET https://store-images.s-microsoft.com/image/apps...?w=176&h=264&q=75&mode=crop&format=jpg` (hero: `w=1280&h=720&q=70`, v0.20.0) | veja abaixo |
| Voltar a jogar | `GET https://<região>/v2/titles/mru?mr=25` com os mesmos headers do `/v2/titles` (Bearer gsToken, `X-Gssv-Client: XboxComBrowser`, `X-MS-Device-Info`) | **NÃO confirmado ao vivo** (veja abaixo) |

- **IDs das SIGLs**: são os que o próprio `https://www.xbox.com/pt-BR/play` carrega hoje. O estado inicial da página tem as chaves `v3_<id>__Cloud:XGPUWEB`, com os títulos "Mais populares na nuvem" e "Adicionados recentemente". A montagem da URL v3 vem do JS do site (`_fetchV3Sigl`).
  Os ids antigos do better-xcloud (`e7590b22…`, `44a55037…`) ainda respondem, mas com listas menores e desatualizadas. O SIGL lista também jogos que a conta não pode jogar; o módulo filtra pelo catálogo do login.
- **store-images**:
  - `w`+`h` sem `mode` encaixa mantendo a proporção: uma BoxArt quadrada vira 176x176.
  - `mode=crop` dá o tamanho exato: preenche e corta no centro.
  - `q` = qualidade JPEG.
  - `format=jpg` é obrigatório: arte de origem PNG volta PNG RGBA de ~550 KB sem ele. `format=jpeg` e `fmt=` dão erro em texto.
  - Saída em JPEG baseline: capa de 8–17 KB (média 12 KB), hero 1280x720 de ~75–120 KB (eram 25–41 KB em 640x360).
  - Resposta com `cache-control: public, max-age=7776000`.
- **Escolha da arte**:
  - capa: Poster > BoxArt > BrandedKeyArt > FeaturePromotionalSquareArt;
  - hero: SuperHeroArt (sem título; o menu escreve o nome) > TitledHeroArt > Screenshot;
  - só imagens do host `store-images.s-microsoft.com`.
- **TLS**: catalog.gamepass.com e store-images.s-microsoft.com usam cadeia ECC Microsoft TLS G2. Os dois validam com as 4 raízes embutidas, também no modo sem hora (bootstrap pelo Date): veja o teste live.
  Medido no host: 1 handshake por host e depois ~170 µs de CPU por capa.
- **MRU**:
  - O caminho `v2/titles/mru?mr=N` vem do JS do xbox.com/play (`enumerateMruTitles`: `${domain}/v${2}/titles/mru?mr=${n}`, `&ct=` para continuação). O stratix usa o mesmo (`getCloudTitlesMRU`, `mr=25`), e a resposta tem o formato do `/v2/titles` (`{"results":[{titleId, details{productId,...}}]}`). O parse reaproveita `xa_parse_titles`.
  - Nunca chamado com token real. A fixture `mru_synthetic.json` é sintética.
  - Na 1ª execução no Xbox, conferir o log `[capas] MRU: N titulos` (ou `MRU falhou: HTTP ...`).

## Cache no pendrive (XC360/, FatFs sem LFN, então tudo 8.3)

- `ROWS.TXT`: `XCCROWS 2` (v0.20.0: nomes em UTF-8; um `XCCROWS 1` antigo, com nomes em ASCII, é ignorado e refeito no login), depois `#ROW <0..3>`, depois linhas `titleId\tbigId\tflags(hex)\tnome`. O ALL do cache também serve de catálogo até o login responder.
- `IMGIDX.TXT`: `XCCIMG 1`, depois `bigId\turls(0/1)\tdisk(bit0 capa, bit1 hero)\tcaminhoCapa\tcaminhoHero`. Os caminhos ficam sem host e sem query.
- `COVERS/XXXXXXXX.JPG`, `HERO/XXXXXXXX.JPG`, onde `XXXXXXXX` = FNV-1a 32 bits do bigId em maiúsculas (o bigId de 12 caracteres não cabe em 8.3).
  O arquivo é o JPEG mais um trailer de 32 bytes (magia + bigId), conferido na leitura: capa `"XCC1"`, hero `"XCH2"` (v0.20.0: um hero 640x360 antigo, com `XCC1`, não confere e é baixado de novo em 1280x720 por cima). Colisão ou arquivo corrompido = baixa de novo. Decoders de JPEG ignoram o trailer.
- Todas as gravações passam pelo `xc_store_save` (`.NEW` + rename). ~40 capas ≈ 0,5 MB.

## Integração (main.c, feita na v0.20.0)

1. `#include "catalog/xc_catalog.h"`. No `menu_main`, **antes** do `lt_start()` do boot: `xcc_init(NULL);`.
2. No loop do menu: `xcc_poll()` todo quadro. Depois `xcc_rows_copy(&rows)`, `xcc_want(...)` com as capas visíveis e o hero do selecionado, e `xcc_cover_get` / `xcc_hero_get` no desenho.
3. Ligar o worker quando a hw 3 estiver livre, no menu e fora do jogo:
   `if (!lt_running() && xn_can_play() && !waiting && !xcc_bg_running()) xcc_bg_start();`. Pode ser tentado a cada ~1 s; ele recusa se a hw 3 estiver ocupada.
4. **PLAY (A)**: se `xcc_bg_running()`, chamar `xcc_bg_stop()` e mostrar "Aguarde..." (igual ao `lt_running()` hoje). Dar o PLAY quando `!xcc_bg_running()`; a parada leva poucos ms. Depois do `xn_play(...) == XN_OK`: `xcc_mru_touch(g->id)`.
5. **Y (relogin)** / qualquer `lt_start()` / `xst_start()`: antes, `xcc_bg_stop()` e esperar `!xcc_bg_running()`.
   Isso é obrigatório: `lt_start` retorna 0 **sem subir a thread** se a hw 3 estiver ocupada.
6. Opcional: `xcc_status()` no overlay de diagnóstico.

Nada mais precisa mudar no login: os ganchos são registrados pelo `xcc_init`.

## Testes (host)

`app/h264play/hosttest/catalog/run_catalog.sh [unit|live|all]`
- **unit**: ASan+UBSan, com fetch falso servindo as fixtures. Resultado: **292/292 checks**. Cobre:
  - parse de SIGL v2/v3/objeto/lixo;
  - displaycatalog cru e enxuto, com preferência de arte e de host;
  - URL de tamanho e nome 8.3;
  - decode: tamanho exato, formato UI_RGB, escala por média de área, corte de quadrada para retrato, truncado/lixo;
  - fileiras: MRU na ordem, sem item fora do catálogo e sem repetido; populares/novos na ordem da SIGL e filtrados; ALL alfabético;
  - prioridade: lote DC e depois as 5 visíveis na ordem, antes de qualquer fundo;
  - fundo: 39/39 no pendrive com só 5 decodes;
  - LRU: cap 6, 10 pedidos geram 6 decodes, despeja primeiro o que está fora do pedido, recarrega do pendrive sem download;
  - hero;
  - `mru_touch`;
  - reinício offline: fileiras e capas vêm do cache sem rede;
  - rede fora: não martela;
  - trailer errado: baixa de novo;
  - 404 com backoff; produto sem imagem nunca é pedido;
  - thread: hw ocupada recusa; stop com pedido em voo leva 7–31 ms; o gancho é desregistrado.
- **live**: SIGL, displaycatalog e store-images reais pelo **nosso** `https.c`, com hora do host e com `XC_TIME_UNKNOWN=1`: PASS nos dois. 4 capas + 1 hero em 0,9–1,9 s.
- Regressão: `hosttest/login/run_login.sh unit` (84/0) e `hosttest/session/run_session.sh unit` (18897/0) continuam passando.
- Xenon: `xenon-gcc` com as flags do Makefile compila `source/catalog/*.c` e os arquivos do login alterados sem warnings novos. **vmxaudit: 0 hits**. Nenhum ELF foi gerado: o build/ e o ELF são do outro agente.

### Fixtures (`hosttest/catalog/fixtures/`, ~1 MB, regenerar com `make_fixtures.py`)

- `sigl_popular_v3_BR.json`, `sigl_new_v3_BR.json`, `sigl_new_v2_BR.json`: respostas reais.
- `displaycatalog_raw3_BR.json`: resposta crua completa, 3 produtos.
- `displaycatalog_BR.json`: 39 produtos, enxuto (sem Screenshots).
- `titles_synthetic.json`, `mru_synthetic.json`: formato do GSSV, com titleIds inventados (`T<bigId>`).
- `covers/<bigId>.jpg`: 39 capas 176x264, mais `covers/INDEX.tsv` (bigId, titleId sintético, nome, fileira).
- `hero/<bigId>.jpg`: 6 heroes 1280x720 (v0.20.0).
- `boxart_square_176.jpg`.

As capas servem pro mockup do agente de UI. São arte das publishers, vinda do CDN público da Store, **só pra teste local e mockup**: não empacotar em release.

## v0.20.0: nomes com acento

- O login (`xbox_auth.c`) guarda os nomes em UTF-8 (`xa_name_clean`: UTF-8 inválido, controle e TM/R/C saem, espaços colapsados, corte sem partir caractere). `xa_ascii_fold` continua só pros textos de erro/estado.
- Nomes pedidos ao displaycatalog em `market=BR&languages=pt-BR` (o mesmo das capas); o que o BR não tiver vai de novo em `market=US&languages=en-US`.
- `NAMES.TXT` agora começa com `XCNAMES 2`. O arquivo antigo (sem cabeçalho, nomes em ASCII) é ignorado: os nomes são buscados de novo e o arquivo é regravado. No 1º boot da v0.20.0 o login demora um pouco mais (um lote de displaycatalog a cada 20 jogos).
