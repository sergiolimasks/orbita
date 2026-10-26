# XCloud360 — atualização pelo próprio Xbox

"Atualizar" no menu baixa a versão nova da ponte `https://x360.pistatrack.com`
e instala no pendrive. Sem PC, sem FTP.

```
menu (hw 0)                 worker hw 3 (xc_update)                       ponte (VPS)          R2 xcloud360-logs
  xcu_check_start ───────▶ GET /v1/update/latest [X-Api-Key beta] ───────▶ stable.json / beta.json
                           confere assinatura ECDSA P-256 + semver > atual
  "Nova versão vX" ◀────── XCU_AVAILABLE (notas, tamanho, canal)
  xcu_install_start ─────▶ GET /v1/update/<v>/xenon.elf  Range 256 KB ───▶ releases/<v>/xenon.elf
                           -> XC360/XENON.NEW (retoma do tamanho atual)
                           sha256 relendo o .NEW do pendrive
                           xenon.elf -> XC360/XENON.OLD ; XENON.NEW -> xenon.elf
  "Reinicie o Xbox" ◀───── XCU_DONE
```

## Arquivos

| arquivo | o quê |
|---|---|
| `source/update/xc_update.{h,c}` | API do menu + núcleo (semver, manifesto, assinatura, download, troca) |
| `source/update/xcu_port.h` | o que o núcleo pede da plataforma (thread, rede, pendrive) |
| `source/update/xcu_port_xenon.c` | Xbox: hw 3 com pilha de 256 KB, FatFs com `xc_usb_hold` curto |
| `source/update/xcu_net.c` | GET HTTPS keep-alive (`https_conn_*`, mesmas raízes do login) |
| `source/update/xcu_pubkey.h` | chave PÚBLICA de release (DER). A privada nunca vai pro git |
| `hosttest/update/` | testes do host (`run_update.sh unit|live`) |
| `tools/release.py` | publica / promove versões (devbox) |
| `tools/beta_keys.py` | chaves de testador beta da ponte (devbox) |

Ponte: `~/scratch/server.py` no devbox = `cc360-vps:/root/xc360-logs/server.py`
(fora do repo; Python stdlib).

## Canais

* **stable** — público. `GET /v1/update/latest` sem chave (ou com chave inválida)
  devolve `releases/stable.json`.
* **beta** — só testadores. Com `X-Api-Key: <chave beta>` válida a ponte devolve o
  MAIS NOVO entre `beta.json` e `stable.json` (testador nunca fica atrás do stable).
* Download: versão que está ou esteve no stable (marcador `releases/<v>/public`,
  posto pelo `release.py promote`) baixa sem chave; versão só do beta exige chave
  beta (401 sem).
* O app manda `X-Api-Key` se existir `XC360/BETAKEY.TXT` no pendrive (lido com
  `xc_store_load`, uma linha, `[A-Za-z0-9-]{8,64}`). Quando a API de beta do menu
  existir, ela só precisa gravar esse arquivo — ou trocar `beta_header()` em
  `xc_update.c` pela chamada dela.
* `?env=test` em qualquer rota = mesmo esquema em `releases-test/` (ensaio).

## Endpoints da ponte

| rota | resposta |
|---|---|
| `GET /v1/update/latest[?env=test]` | 200 manifesto (bytes exatos do R2), 404 sem release, 429 rate limit |
| `GET /v1/update/<v>/xenon.elf[?env=test]` | 200/206 stream do R2 (`Range: bytes=a-b`, `Content-Range`, `Accept-Ranges`), 401 beta sem chave, 404, 416 |
| `GET /v1/beta/check` + `X-Api-Key` | 204 chave válida / 401 (30/min por IP) |
| `POST /v1/logs` + `X-Api-Key` | igual antes; aceita qualquer chave beta e grava `"tester": <nome>` no lote |
| `GET /health` | 200 |

Rate limit das rotas de update: 240 pedidos/min por IP (um ELF de 5 MB = 20 pedaços).
Manifestos e marcadores ficam 60 s em cache na ponte (uma publicação leva até 1 min
pra aparecer).

## Manifesto (`releases/stable.json`, `releases/beta.json`, `releases/<v>/manifest.json`)

```json
{
 "format": 1,
 "channel": "beta",
 "version": "0.21.0-beta.1",
 "size": 4946760,
 "sha256": "<64 hex minúsculos>",
 "url": "/v1/update/0.21.0-beta.1/xenon.elf",
 "notes": "Correções do áudio e menu novo",
 "sig": "<base64 da assinatura DER ECDSA P-256/SHA-256>"
}
```

Bytes assinados (o app remonta exatamente isto — `xcu_manifest_message`):

```
xc360-update-v1\nchannel=<c>\nversion=<v>\nsize=<n>\nsha256=<hex>\nurl=<url>\nnotes=<notas UTF-8>
```

O app recusa: formato ≠ 1, canal fora de stable/beta, semver inválida, tamanho fora
de 64 KB..48 MB, `url` que não seja `/v1/update/<a mesma versão>/xenon.elf[?env=test]`,
assinatura que não bate com `xcu_pubkey.h`. Depois do download o sha256 é
recalculado lendo o arquivo DO PENDRIVE (pega também erro de gravação no USB) e o
começo tem que ser `\x7fELF`. Downgrade não acontece: só instala versão maior que a
atual (um manifesto velho reaproveitado é ignorado).

Semver: `0.21.0-beta.1 < 0.21.0-beta.10 < 0.21.0 < 0.21.1`. Sufixo de variante do
Makefile (`0.20.0-full`, `-trace`) é ignorado na versão atual; pre-release de
verdade começa com `alpha`, `beta`, `rc` ou `test`.

## Chaves

* Release (assinatura): `~/credentials/xc360-release-p256.pem` no devbox (chmod 600,
  NUNCA no git). Pública em `source/update/xcu_pubkey.h`. Trocar a chave = os
  Xbox com o app antigo param de aceitar atualização (reinstalar pelo PC).
* Testadores beta: mestre em `~/credentials/xc360-beta-keys.tsv` (chmod 600;
  `chave<TAB>nome`). Na VPS: `/root/xc360-logs/beta/keys.tsv`, pasta montada
  `ro` em `/run/beta` no container; a ponte relê sozinha quando o arquivo muda
  (≤ 5 s, ou `SIGHUP`), sem reiniciar. Sem o arquivo, só a chave do segredo
  `xc360_logs_key` vale (como "dev").

```
tools/beta_keys.py new joao        # gera XXXXX-XXXXX (sem 0/O/1/I/L) e publica
tools/beta_keys.py list [--show]
tools/beta_keys.py revoke joao     # (ou pela chave) e publica
tools/beta_keys.py check <chave>   # GET /v1/beta/check
```

A ponte ignora `-`, espaços e maiúsculas/minúsculas na chave.

## Publicar uma versão (`tools/release.py`, no devbox)

```
# 1. beta (padrão): só testadores veem
tools/release.py publish --elf app/h264play/h264play.elf32 --version 0.21.0-beta.1 \
    --notes "Menu novo e atualização pelo Xbox" [--github]
# 2. virou estável: copia pro stable (reassina com channel=stable; o ELF fica público)
tools/release.py promote 0.21.0-beta.1 [--github]
# (ou direto no stable: publish --channel stable)
tools/release.py show              # manifestos atuais + assinatura conferida
```

* Recusa versão ≤ a atual do canal (`--force` pra passar por cima), ELF que não é
  PowerPC big-endian e notas > 480 bytes. `--dry-run` só mostra o manifesto.
* `--github` (precisa do `gh` no host, hoje não instalado no devbox): cria a
  Release `vX.Y.Z` com `xenon.elf` + `xenon.elf.sha256` (beta = `--prerelease`);
  `promote --github` tira o prerelease.
* Ensaio: `--test` em tudo usa `releases-test/` (a ponte serve com `?env=test`);
  `tools/release.py rm-test` limpa.
* A versão publicada TEM que ser a mesma do `XCLOUD360_BUILD_VERSION` do ELF, senão
  o Xbox oferece a atualização de novo depois de instalar.

## Segurança do pendrive

Ordem da troca (`swap_in`), com rollback a cada passo:

1. `.NEW` completo e conferido (sha256 + `\x7fELF`). Antes disso o `xenon.elf` nem
   é tocado; download interrompido / cancelado deixa `XENON.NEW` + `XENON.INF`
   (sha/tamanho/versão) e a próxima tentativa continua do tamanho do `.NEW`
   (descarta se o `.INF` for de outra release).
2. apaga o `XENON.OLD` antigo; `xenon.elf → XC360/XENON.OLD`. Falhou = nada mudou.
3. `XC360/XENON.NEW → xenon.elf` e confere o sha256 do `xenon.elf`. Falhou =
   devolve o OLD por rename; se o rename falhar, por cópia; se nem isso, instala o
   NEW verificado por cópia. Pior caso (USB morrendo): erro "GRAVE" na tela, e o
   OLD e o NEW continuam no pendrive pra copiar no PC.
4. Sem espaço pro download, o `XENON.OLD` antigo é apagado primeiro (o `xenon.elf`
   atual nunca).

**Queda de energia**: o FAT não tem rename atômico por cima de um arquivo existente,
então entre o passo 2 e o 3 (duas gravações de diretório, poucos ms) o pendrive fica
sem `xenon.elf`. Se a energia cair EXATAMENTE aí, o XeLL não acha o que bootar: copie
`XC360/XENON.OLD` (ou o `.NEW`) pra raiz como `xenon.elf` no PC, ou suba o app pela
rede — o próximo "Procurar atualização" (`recover_elf`) restaura o `xenon.elf` do
`XENON.OLD` sozinho. Dentro de cada rename o FatFs cria a entrada nova antes de
apagar a velha, então ali nunca fica sem nenhuma das duas.

USB: cada operação segura o USB (`xc_usb_hold`) só por um pedaço de 32 KB, então a
thread de rede volta a ler o controle entre um e outro (B pra cancelar funciona).

## API pro menu (`xc_update.h`)

```c
void xcu_init(const char *current_version, const xcu_config_t *cfg);  /* cfg NULL = padrão */
int  xcu_check_start(void);     /* 0 = subiu o worker; -1 = hw 3 ocupada / já roda */
int  xcu_install_start(void);   /* precisa de XCU_AVAILABLE (ou FAILED/CANCELLED depois dele) */
void xcu_cancel(void);          /* não bloqueia; ignorado em XCU_INSTALLING */
int  xcu_running(void);
void xcu_snapshot(xcu_snap_t *s);
```

`xcu_snap_t`: `phase` (`XCU_IDLE, CHECKING, UP_TO_DATE, AVAILABLE, DOWNLOADING,
VERIFYING, INSTALLING, DONE, FAILED, CANCELLED`), `seq` (muda = redesenhar), `busy`,
`cancellable`, `current`, `latest`, `channel`, `notes` (UTF-8), `done/total/pct` da
fase (download ou verificação), `status` (linha pt-BR), `err`.

## Integração (quem mexe em main.c / source/ui / Makefile)

1. **Makefile** — junto do catálogo, dentro de `XBOX_LOGIN=1`:
   `SOURCES += source/update`. (Com o `--gc-sections`, nada entra no ELF enquanto o
   main não chamar.) Exige `make clean` na primeira vez.
2. **Boot** (`main.c`, depois do `xcc_init`):
   `xcu_init(XCLOUD360_BUILD_VERSION, NULL);`
3. **Quando checar**: a hw 3 é compartilhada. Só chame `xcu_check_start()` /
   `xcu_install_start()` com `!lt_running() && !xcc_bg_running()` e sem sessão
   (`xn_can_play()`), nunca durante o jogo. Sugestão: checar uma vez depois que o
   login terminar (antes de `xcc_bg_start`), e de novo quando o usuário abrir
   "Atualizar". Se o catálogo estiver baixando capas: `xcc_bg_stop()`, esperar
   `!xcc_bg_running()`, então `xcu_*_start()`; religar o catálogo quando
   `!xcu_running()`. Enquanto `xcu_running()`, bloquear PLAY e `lt_start` (como já
   é feito com o login).
4. **Menu (estilo XUI)**: item "Atualizar" no fim da home/configurações:
   * `XCU_AVAILABLE` → selo "Nova versão vX (beta)" no item; A abre a tela.
   * `XCU_UP_TO_DATE` → "Você já está na versão mais nova (vX)".
   * Tela de atualização: título "Atualizar para vX", `notes` (UTF-8;
     `xui_utf8_to_latin1`), tamanho (`total/1024` KB), "A instalar · B voltar".
5. **Tela de progresso** (mesmo fundo/tipografia do menu novo): `status` em cima,
   barra com `pct` e "`done/1024` de `total/1024` KB" em `DOWNLOADING`, "Conferindo"
   em `VERIFYING`, "Instalando — não desligue o Xbox" em `INSTALLING` (sem B).
   Rodapé "B cancelar" só se `cancellable` (→ `xcu_cancel()`; o download retoma da
   próxima vez). Redesenhar quando `seq` mudar.
6. **Fim**: `XCU_DONE` → "Atualizado para vX. Reinicie o Xbox." com A =
   reiniciar (o mesmo caminho de reboot/power-cycle que o menu já usa) e B = depois.
   `XCU_FAILED` → `err` + "A tentar de novo" (`xcu_install_start()`; continua de
   onde parou). `XCU_CANCELLED` → volta pra tela anterior.
7. **Chave beta**: a tela de chave do outro módulo grava `XC360/BETAKEY.TXT` e
   pode validar com `GET /v1/beta/check`. Depois de gravar, chamar
   `xcu_check_start()` de novo (o testador pode ganhar uma versão beta).

## Testes

```
app/h264play/hosttest/update/run_update.sh unit     # sem rede, ASan+UBSan
XCU_BETA_KEY=<chave> app/h264play/hosttest/update/run_update.sh live beta-only|promoted
```

* **unit** (137 checagens): semver; manifesto válido/malformado (url de outra
  versão, host externo, sha curto, canal estranho...); assinatura válida, adulterada
  (notas, tamanho, sha, canal), de outra chave e contra a chave de release; check
  (com/sem chave beta no header, sem release, variante `-full`); instalação feliz
  (e de novo com `XENON.OLD` existente); pendrive "arrancado" no meio do download +
  retomada do byte certo; rede caindo (tentativas) e morta (falha, retoma depois);
  cancelar no meio + retomar; `.NEW` de outra release descartado; sha errado;
  beta sem chave (401); sem espaço; falha no 1º rename, no 2º (rollback por
  rename), em todos (rollback por cópia), renames e cópias (erro grave sem apagar
  nada); pendrive sem `xenon.elf` restaurado do `XENON.OLD`. Chave ECDSA de TESTE
  gerada na hora.
* **live**: contra a ponte de produção com `?env=test`, o HTTPS do app (raízes
  embutidas, também sem hora — bootstrap pelo Date) e a chave pública embutida.
  `beta-only`: sem chave nada; com chave aparece; baixar sem chave = 401; com chave
  instala. `promoted`: sem chave aparece e instala. Precisa de um release de teste
  publicado:

```
tools/release.py publish --test --elf <ELF qualquer PPC> --version 0.0.0-test --notes teste
# (esperar 1 min do cache) run_update.sh live beta-only
tools/release.py promote 0.0.0-test --test
# (esperar 1 min) run_update.sh live promoted
tools/release.py rm-test
```

## v0.20.3: X-Console e atualização obrigatória

- Com chave beta, todo pedido leva também `X-Console: <ID de hardware>`
  (`xcu_set_console_id`, ver BETA-GATE.md). Chave inválida = a ponte serve o stable.
- 404 no `/latest` (nada publicado) = "sem atualização", não erro.
- Manifesto ganhou `"mandatory": true|false` + `"sig2"` = assinatura de
  `<bytes assinados de sempre>\nmandatory=<1|0>`. O `"sig"` antigo não muda (apps
  até a v0.20.2 ignoram os campos novos e continuam atualizando). No app
  (`xcu_manifest_mandatory`, `xcu_snap_t.mandatory`): campo ausente, sem `sig2`
  ou `sig2` que não confere = **obrigatória**; só `false` com `sig2` válido é opcional.
- `tools/release.py publish ... [--optional]`: padrão obrigatória; `promote`
  mantém o campo (reassina `sig` e `sig2`); `show` diz obrigatória/opcional.
- No app: procura logo na entrada (depois da chave beta, antes do login); popup
  com "Atualizar agora" (obrigatória: sem B em nenhuma fase) ou
  "Atualizar agora / Depois". Falha de rede = segue e tenta de novo em 10 min.
- Testes: `run_update.sh unit` 161/161 (+ fixtures `mand/opt/opt_tampered/opt_nosig2`,
  promote preservando, `sig` legado ainda válido, X-Console junto da chave).
