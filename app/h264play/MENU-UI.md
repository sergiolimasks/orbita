# Menu novo (XUI): visual do site xCloud

Status (v0.20.0, 2026-10-25): **integrado no `main.c` e padrão** (o flag `XUI` saiu do Makefile; `source/ui` sempre entra no ELF). O menu de texto antigo (`menu_ui.c`) foi removido. O visual foi aprovado pelos PNGs abaixo; a v0.20.0 acrescenta a versão ("v0.20.0") ao lado do logo (`xui_home_data_t.version`) e o hero em 1280x720 (cópia 1:1, nítido).

## Ver o visual

```sh
app/h264play/hosttest/ui/run.sh      # (também roda dentro do hosttest/run.sh)
```

Os PNGs (1280x720) saem em `app/h264play/hosttest/ui/out/`:
- home: `01_home`, `02_home_fileira2`, `04_home_rolada`, `06_todos_os_jogos`;
- quadros no meio de animação: `03_rolagem_no_meio`, `05_troca_de_fileira_no_meio`;
- capa sem imagem (placeholder): `07_placeholder_sem_capa`;
- sessão aberta: `08_sessao_aberta`, `09_sessao_aberta_outro_foco`;
- botões de energia no topo: `10_topo_reiniciar`, `11_topo_desligar`;
- confirmação: `12_confirmar_desligar`, `13_confirmar_desligar_sim`, `14_confirmar_reiniciar`;
- telas de estado: `15_login`, `16_carregando_catalogo`, `17_fila`, `18_iniciando`, `19_erro`, `20_desligando`;
- sem imagem nenhuma e catálogo vazio: `21_sem_imagens`, `22_catalogo_vazio`;
- `bench.txt`: custo por quadro.

Os dados são as fixtures da camada de catálogo (`hosttest/catalog/fixtures`: 39 capas 176x264 e 6 heroes 1280x720 desde a v0.20.0), decodificadas pelo `xcc_decode_jpeg` real. Sem as fixtures, o mockup roda só com placeholders.

## Arquivos

| arquivo | o que é |
|---|---|
| `source/ui/xui.h` | API: canvas XRGB, primitivas, texto, dados que o desenho consome, telas |
| `source/ui/xui_gfx.c` | retângulo/cantos AA, degradê, máscara, escala (cópia 1:1, média de bloco ou bilinear, aceita pixels UI_RGB) |
| `source/ui/xui_text.c` | texto com a Inter: UTF-8 → Latin-1, kerning, "…", quebra de linha |
| `source/ui/xui_home.c` | home (hero + fileiras + botões do topo + diálogo), telas de estado, caches, animação, retângulo sujo |
| `source/ui/xui_tile.c` | canvas → fb em tiles 32x32 do Xenos, já na ordem de bytes certa |
| `source/ui/xui_mm.{c,h}` | `menu_model` → `xui_home_data_t` |
| `source/ui/xui_cat.{c,h}` | catálogo (`xc_catalog.h`) → fileiras do modelo, provedor de capas/hero, `xcc_want` |
| `source/ui/xui_fb_xenon.{c,h}` | backend do Xbox: canvas em RAM → fb do console (só o retângulo sujo) |
| `source/ui/xui_assets.h` | **gerado** por `tools/gen_xui_assets.py`: atlas da fonte e ícones |
| `third_party/inter-4.1/` | TTFs da Inter (OFL 1.1), só entrada do gerador; veja `THIRD_PARTY.md` |
| `hosttest/ui/` | `xui_test.c` (unidade), `mockup.c` (PNGs + bench + teste de sujeira), `xui_png.c`, `run.sh` |

Fonte: Inter Regular, SemiBold e Bold, rasterizadas com anti-aliasing em 14, 17, 21, 26 e 38 px (mais 60 px só com dígitos e A–Z, pro código de login). Ícones (bola com X, desligar, reiniciar, controle, selo GAME PASS) são desenho geométrico do gerador, sem nenhum arquivo de marca. `make ui-assets` regenera tudo, e `make XUI=1` regenera quando o gerador muda.

## Navegação (menu_model, testado em `hosttest/menu_test.c`)

- Fileiras: `rows[]`, cada uma com índices em `games[]`. Cada fileira lembra a própria coluna.
  - `mm_nav(dx, dy)`: esquerda/direita para nas pontas; cima/baixo troca de fileira e volta na coluna lembrada.
  - Subir além da 1ª fileira vai para os botões do topo (`row = MM_ROW_HEADER`, `hcol` = Reiniciar/Desligar).
- `mm_nav_page` (LB/RB) pula vários cards.
- `mm_press_a`:
  - numa capa: `MM_A_PLAY`;
  - num botão do topo: abre a confirmação (`MM_A_CONFIRM`), com **Cancelar** em foco;
  - na confirmação: `MM_A_REBOOT`/`MM_A_SHUTDOWN`, que vão direto pro `mm_power_run` de sempre (encerra a sessão com timeout de 3 s e chama o SMC).
- `mm_press_b` fecha a confirmação ou desce do topo.
- O END do catálogo monta "Voltar a jogar" + "Todos os jogos" por padrão. O `xui_cat_sync` troca isso pelas 4 fileiras do catálogo (MRU, populares, novos, todos), mantendo o foco no mesmo jogo (`mm_rows_done`).
- Nomes de jogo guardam UTF-8 válido de ponta a ponta (v0.20.0: o login também, ver CATALOG-COVERS.md).
- v0.20.0: o menu de energia por Start e a lista de texto (`mm_move`/`mm_page`/`mm_letter`, `mm_power_open`...) saíram do modelo. No menu novo, o Start não abre nada.

## Desempenho: CPU, não GPU

Medido no host (`bench.txt`, -O2, desenho + cópia em tiles):

| quadro | host | Xbox (estimado 5–10x) |
|---|---|---|
| rolagem horizontal (faixa ~510k px) | ~1,1–1,5 ms | ~6–15 ms |
| troca de fileira (tela inteira, animada) | ~1,9–2,2 ms | ~10–22 ms |
| foco anda 1 card sem rolar | ~0,8 ms | ~4–8 ms |
| tela de estado: pontinhos (26k px) | 0,15 ms | ~1 ms |
| cópia em tiles 1280x720 | 0,7 ms | ver nota abaixo |
| escalar 1 capa 176x264 → 148x222 | 0,32 ms | ~2–3 ms |
| hero 1280x720 → 1280x500 (cópia 1:1, v0.20.0) | ~1,3 ms | ~7–13 ms |

Notas:
- **Cópia em tiles no Xbox**: o fb é não-cacheado. O `ui_clear` antigo levava ~15 ms para 3,7 MB com stores de 4 B. Aqui são stores de 8 B e só o retângulo sujo: estimativa de ~3–4 ms para a faixa da rolagem.
- **Hero e capas**: o hero só é refeito quando o foco **para** num jogo (220 ms sem mexer), então segurar o direcional não escala imagem nenhuma. As capas ficam num cache LRU de 96 já escaladas, e com a tela parada são pré-escaladas 2 por quadro (fileira atual e vizinhas). Assim a rolagem não paga escala.
- **Animação**: depende do tempo (`dt`), não do número de quadros. Se o Xbox desenhar a 30 fps, a rolagem continua com a mesma duração, só com menos quadros.
- **Durante o jogo**: nada é desenhado (o desenho só acontece dentro do loop do menu). O XUI não tem thread nem timer.

Por que não a GPU (`gpu_present.c`):
- **Shader**: o caminho de GPU que existe só tem o shader YUV420→RGB do xmplayer (microcódigo pré-compilado). O libxenon não compila shader em runtime, e não há um shader RGB pronto no repo.
- **YUV**: usar o shader que existe exigiria converter o menu para YUV na CPU a cada quadro. Isso custa mais que a própria cópia em tiles (0,7 ms no host pela tela inteira, e a rolagem só copia ~55% disso) e ainda borra o texto pela subamostragem de croma.
- **Scanout**: o menu hoje roda com o scanout devolvido ao console (`gpu_present_finish`). Desenhar o menu na GPU misturaria o estado do vídeo com o do menu, justo onde o jogo volta.

O desenho na CPU, só no retângulo sujo, cabe no orçamento. Se medir mal no Xbox, o próximo passo é reduzir a faixa da rolagem, não trocar para GPU.

## Dados que o desenho consome (`xui_home_data_t`)

- **fileiras**: `xui_row_t { title, items[] { id, name (UTF-8), flags, game } }` e o foco `(row, col)`. `row = -1` são os botões do topo.
- **imagens**: `cover(u, id)` / `hero(u, id)` devolvem `xui_image_t` ou NULL. NULL = placeholder cinza com o nome, ou hero borrado a partir da capa. Os pixels podem estar em `XUI_FMT_UIRGB` (o formato do catálogo), e a conversão sai de graça na escala.
- **`img_gen`**: quando muda, o XUI reescala. O `xui_cat` usa `xcc_seq()`.
- **sessão aberta** (o jogo continua no servidor ao sair pro menu): `session_id` / `session_name`. A capa ganha o selo "EM JOGO" e o hero mostra "Sessão em andamento · A: voltar ao jogo". No main: `xc_game_session(&gs)` (`source/game_session.h`) a cada quadro; com `gs.state == XC_GS_OPEN`, `xd.session_id = gs.id` e `xd.session_name` = nome do jogo (`g_mm.games` pelo id). O XUI não pergunta nada à sessão.
- **`hint`** ("A:Jogar|Y:Atualizar") e **`notice`** (aviso curto sob o hero).
- **telas de estado**: `xui_status_t { kind, title, text, url, code, detail, hint, game_id, progress }`. Os tipos são LOADING, LOGIN, STARTING, QUEUE, ERROR e BUSY.

## Integração no main.c (feita na v0.20.0; o texto abaixo é o roteiro que foi seguido)

1. `make XUI=1` (põe `source/ui` no build). Estado estático: `xui_view_t *xv` (`xui_view_new(1280,720)`, ~16 MB), `xui_mm_t xmm`, `xui_cat_t xcat`, `xui_home_data_t xd`. No boot: `xui_cat_init(&xcat); xui_cat_bind(&xcat, &xd);`, junto do `xcc_init` (CATALOG-COVERS.md).
2. Entrando no `menu_loop` (depois do `gpu_present_finish`): `xui_fb_attach(); xui_view_invalidate(xv);`.
3. Em cada volta do loop:
   - `xcc_poll()`;
   - `xui_cat_sync(&xcat, &g_mm, &xd)`;
   - `xui_mm_sync(&xmm, &g_mm, &xd, xui_cat_flags, &xcat)`;
   - preencher `xd.session_*`, `xd.hint`, `xd.notice`;
   - na lista: `r = xui_home_frame(xv, xui_fb_canvas(), &xd, xui_mm_confirm(&xmm, &g_mm), ms, &anim)`;
   - nas outras telas: `xui_status_frame(...)`, com o `xui_status_t` montado a partir do que o `menu_status` já calcula;
   - depois: `xui_fb_present(r)` e `xui_cat_want(&xcat, xv, &xd)`;
   - se `anim == 0`, pode dormir até o próximo input.
4. Controles:
   - D-pad/stick → `mm_nav` (mesmo `key_repeat`);
   - LB/RB → `mm_nav_page(±1, 6)`;
   - A → `mm_press_a`. `MM_A_PLAY` segue o PLAY de hoje: antes, `xcc_bg_stop()` e esperar `!xcc_bg_running()` (regra da hw 3). `MM_A_REBOOT`/`MM_A_SHUTDOWN` → `menu_power(MM_PWR_REBOOT|MM_PWR_SHUTDOWN)` como hoje;
   - B → `if (!mm_press_b(&g_mm))` mantém o B de hoje;
   - **tirar o `mm_power_open` do Start**.
   - O `show()` do `mm_power_run` desenha `XUI_ST_BUSY`.
5. Não mexer em `video_teardown` / `ingame_check` / `run_video` (outro agente). O menu só desenha no `menu_loop`.
6. O rodapé de log do menu antigo (`ui_log_*`) não existe no visual novo. Os `printf` continuam indo pro hook/UART. Dá pra pôr de volta como overlay de diagnóstico, se for preciso.

Resolvido na v0.20.0:
- **acentos no Xbox**: o login guarda os nomes em UTF-8 (`xa_name_clean`, nomes em pt-BR com en-US de reserva) e o cache `NAMES.TXT`/`ROWS.TXT` foi versionado pra rebaixar com acento.
- **hero**: o catálogo pede 1280x720 (q=70) e o XUI só copia.

Como o `main.c` usa (v0.20.0):
- telas: `SCR_LIST` (home), `SCR_LOGIN` (código), `SCR_LOADING` (login/catálogo em andamento, sem jogos ainda), `SCR_FAIL` (login falhou: Y tenta de novo — não existe mais "usar o catálogo do PC"), `SCR_WAIT` (iniciando/fila, B cancela), `SCR_NONET`;
- A num jogo: entra quando o worker do catálogo largou a hw 3 (`xcc_bg_stop` + `!xcc_bg_running`) e a sessão anterior fechou; o mesmo jogo aberto volta na hora (`xn_resume`);
- Y e X (teste offline) também esperam o worker largar a hw 3;
- Reiniciar/Desligar: botões do topo -> confirmação -> `mm_power_run` (a plataforma também para o worker do catálogo antes do SMC; a tela `XUI_ST_BUSY` mostra o andamento);
- stdout: com o menu na tela, o `printf` vai só pra UART (o console do libxenon desenharia no mesmo fb).

## v0.20.1: marca orbita, ASSINATURA, botão Atualizar

- **Marca** (brand/BRAND.md): o logo "bola com X" + "CLOUD GAMING" saiu. O topo de
  todas as telas (home e estados) mostra o logo `orbita` colorido, 32 px de altura em
  (64, 21), gerado por `tools/gen_xui_assets.py` a partir de
  `brand/png/orbita-horizontal-256h.png` como sprite RGBA (`xui_sprite_t`,
  `xui_sprite()`), e a versão em Haze (#9AA0A6) 12 px depois.
- **Destaque** Orbita Sky (#38B6FF, `XUI_C_ACCENT`) no lugar do verde Xbox: selo
  "EM JOGO", ponto/lembrete de sessão aberta, posição na fila, barra de progresso e
  pontinhos. O botão A continua verde (cor do controle), B/X/Y também.
- **Selo GAME PASS** (marca da Microsoft) saiu: jogo da assinatura (`XUI_IT_GAMEPASS`,
  `XA_F_SUB`) ganha a etiqueta **ASSINATURA** no mesmo lugar (capa, canto de baixo;
  hero, antes do "Jogue na nuvem"). Jogo fora da assinatura (free-to-play, "compre
  para jogar") fica sem etiqueta.
- Textos: "orbita" no login ("Pra jogar no orbita...") e no banner do boot; "xCloud"
  saiu dos textos da tela ("sessão na nuvem").
- **Atualizar** (source/update, UPDATE.md): terceiro botão do topo, à esquerda
  (`MM_HB_UPDATE`/`XUI_HB_UPDATE`; o foco ao subir continua no Reiniciar). Com
  versão nova: ponto Flare (#FF6B3D) no botão e rótulo "Atualizar: vX". A abre a tela
  de atualização (`SCR_UPDATE` no main): procurar -> "Atualizar para vX" com notas e
  tamanho (A instalar / B voltar) -> barra de progresso com "B Cancelar" quando
  `cancellable` -> "Instalando — não desligue o Xbox" -> "Atualizado para vX.
  Reinicie o Xbox" (A reinicia, B depois). Falha: erro + "A tentar de novo".
  Mockup: `23_topo_atualizar_nova_versao`, `24_atualizar_oferta`, `25_atualizando`,
  `26_atualizado`.
- Regra da hw 3 no main: o app procura atualização sozinho uma vez depois do login;
  `xcu_*_start` só com `!lt_running()`, sem sessão, fora do jogo, depois de
  `xcc_bg_stop` + `!xcc_bg_running()`. Com `xcu_running()` o worker do catálogo não
  sobe e PLAY / Y / X ficam bloqueados.

## v0.20.3: chave beta, Configurações, popup de atualização

- Botão **Configurações** (engrenagem, `XUI_HB_SETTINGS`/`MM_HB_SETTINGS`) à
  esquerda do Atualizar, só com chave beta válida (`xui_home_data_t.settings_avail`,
  `menu_model_t.settings_on`; sem ela o foco nunca chega nele).
- Telas novas: `xui_key_frame` (teclado da chave) e `xui_settings_frame`.
  Mockups `27_` a `40_` (ver BETA-GATE.md). Popup de atualização: `35_`–`37_`.
