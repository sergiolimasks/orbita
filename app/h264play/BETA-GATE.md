# Chave beta presa ao console (v0.20.3)

Build `BETA=1` (padrão do Makefile): sem chave beta válida **o app não abre**
(nem login, nem capas, nem log). A futura versão estável pública compila com
`make BETA=0` (sem chave, sem tela; `make clean` ao trocar). Código:
`source/beta/` (`xc_beta.{h,c}` núcleo portável, `xcb_port_xenon.c`
plataforma, `xc_keypad.{h,c}` teclado na tela); desenho em `source/ui/xui_home.c`
(`xui_key_frame`, `xui_settings_frame`); fluxo no `main.c` (`beta_poll`).

## Identidade do console

`id = sha256("orbita-console-id-v1" || secotp linhas 0..6 || MAC)[:16 bytes]` em
32 hex minúsculos. Linhas 0..6 = tipo + CPU key (as 7..11 são o contador de
updates do dashboard, que muda: ficam fora). MAC = config da NAND
(`xenon_config_get_mac_addr`, o mesmo do enet). Calculado uma vez no boot,
depois da rede. Não existe arquivo de ID (o `CONSOLE.ID` aleatório saiu): o ID
não viaja com o pendrive. Fusíveis/MAC nunca vão pro log nem pra tela.

- Na tela: curto `A1B2-C3D4-E5F6` (12 primeiros hex); nas Configurações também os 32.
- O mesmo ID vai no campo `console` dos logs e no header `X-Console` de **todo**
  pedido com chave: `/v1/beta/check`, `/v1/logs`, `/v1/update/*`.

## Validação

`GET https://x360.pistatrack.com/v1/beta/check` com `X-Api-Key` + `X-Console`
(worker na hw 3, `https_request`, raízes embutidas):

| resposta | app |
|---|---|
| 204 | ok; grava `BETAKEY.TXT` (se digitada) e `BETAOK.TXT` |
| 401 | "Chave inválida" e volta pra digitação |
| 403 `revoked` | "Chave revogada. Fale com o administrador." |
| 403 `duplicate` | "Esta chave é exclusiva de um console e foi bloqueada por uso em outro aparelho. Fale com o administrador." (sem acesso; A = digitar outra) |
| 429 / 5xx (503 `unavailable`) / rede | sem conexão → regra dos 7 dias (só no boot) |
| 400 (`console`) e outros | erro com o código, "Tentar de novo" |

Boot: valida a cada boot, **antes** do login Microsoft. Sem `BETAKEY.TXT` → teclado.

### Sem conexão: 7 dias

`XC360/BETAOK.TXT` = `XCBOK1 <unix> <hmac>` com
`HMAC-SHA256(k, "xcbok1|<id>|<chave canônica>|<unix>")`, onde
`k = sha256("orbita-beta-ok-hmac-v1" || fusíveis || MAC)` (derivada do mesmo
hardware, nunca mostrada; o ID da tela não assina nada). Vale se: assinatura
confere pra ESTE console e ESTA chave, a hora é conhecida (Date do servidor, ou
RTC posterior à build) e `0 ≤ agora − última ≤ 7 dias` (10 min de folga pra
relógio atrasado). Editar à mão / copiar de outro console / outra chave = não vale.
Limite conhecido: sem rede, a hora vem do RTC do Xbox; quem atrasar o relógio do
console estende a janela (o app recusa só relógio que voltou para antes da validação).

## Telas (mockups em `hosttest/ui/out/`)

- `32_validando_boot`: spinner enquanto valida no boot.
- `28_chave_beta`, `29_chave_beta_invalida`, `30_chave_beta_completa`,
  `31_chave_validando`: teclado (alfabeto `23456789ABCDEFGHJKMNPQRSTUVWXYZ`, 4×8
  + Apagar / Do pendrive / Confirmar), `XXXXX-XXXXX` com hífen automático; com as
  10 letras o foco pula pro Confirmar. A digita, B apaga, Y lê
  `XC360/BETAKEY.TXT` (colado pelo PC, qualquer chave 8–64 ASCII, ex. a de dev),
  Start confirma. Só grava se 204.
- `33_chave_duplicada`, `34_chave_revogada`, `38_chave_sem_conexao`.
- `27_topo_configuracoes` (engrenagem à esquerda do Atualizar). v0.21.33: as
  Configurações viraram menu lateral (`60_cfg_conta` em diante, MENU-UI.md); a
  seção **Conta e sobre** tem o ID curto e completo, a versão, a linha **Chave
  beta** (`*****-**P9H · validada em 25/10 22:56`, A = Trocar chave, B volta pra
  seção) e "Para reportar um bug, envie uma foto desta tela junto". Com BETA=0 as
  Configurações existem e a linha da chave some.
- Trocar chave: precisa da hw 3 (para o worker das capas; jogo aberto = "Feche o
  jogo aberto"). Falhou = a chave antiga continua valendo; B volta. Deu certo =
  log passa a usar a nova e o app procura atualização de novo (canal pode mudar).

## Atualização obrigatória (mesma versão)

Logo na entrada (depois da chave, antes do login) o app procura atualização.
Versão nova → popup (`35_atualizacao_obrigatoria`, `36_atualizacao_opcional`,
`37_obrigatoria_reinicie`): obrigatória = só "Atualizar agora" (sem B em
nenhuma fase até reiniciar); opcional = "Atualizar agora" / "Depois" (não volta a
abrir sozinho pra mesma versão). Falha de rede/servidor na checagem = segue pro
login/menu e tenta de novo em 10 min. 404 (nada publicado) = sem atualização.
Manifesto: ver UPDATE.md ("mandatory" + "sig2").

## Testes

`hosttest/beta/run_beta.sh [unit|fake|live|all]` (padrão unit + fake):
- unit (ASan+UBSan): ID estável e diferente pra fusível/MAC diferentes, nada em
  claro; chave (formato, máscara, classificação 204/401/403/400/429/5xx/rede);
  teclado (navegação, pontas, fileira de 7, ações, hífen, apagar, 11ª ignorada,
  Confirmar só com 10); BETAOK (outra grafia, outra chave, outro console, data e
  HMAC editados); 7 dias (exato, +1 s, sem hora, relógio voltou); estados com
  transporte falso (headers `X-Api-Key`+`X-Console`, 401 não grava, 204 grava,
  revoked, duplicate, sem rede ≤7/>7 dias, 502 = sem conexão, adulterado, pendrive
  de outro console, digitada sem rede não grava, 400, ler do pendrive); worker em thread.
- fake: `fake_server.py` (HTTP local) com o contrato: vincula no 1º uso,
  2º console → duplicate e a chave cai pros dois, revoked, 401, 400 sem
  X-Console, 503 → janela de 7 dias.
- live: `x360.pistatrack.com` pelo nosso `https.c` com a chave de DEV
  (`~/credentials/xc360-logs-key`, não vincula console): 204 + `BETAOK.TXT` com a
  hora do Date (também sem hora do host), chave falsa 401, sem X-Console 400.
