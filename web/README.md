# Site da beta (Orbita) — https://x360.pistatrack.com

Flask + Jinja + Postgres. Página pública, login único (admin/testador), convites,
chaves beta, relatos com fotos (R2), painel admin (testadores, convites, relatos,
logs do R2, versões, números). pt-BR / en / es.

## Onde fica cada coisa
- Código: `web/xc360site/` (rotas em `views/`, esquema em `schema.sql`, traduções em `i18n.py`).
- **Marca num lugar só**: `xc360site/brand.json` (nome, logo, cores → `/brand.css`) + arquivos em
  `xc360site/static/brand/` (copiados de `brand/svg` e `brand/png`). Telas em `static/img/`
  (geradas de `app/h264play/hosttest/ui/out/`).
- Deploy: `sh web/deploy/deploy.sh` (no devbox, raiz do repo). Copia `web/` pra
  `cc360-vps:/root/xc360-site/src`, builda `xc360-site:<tag>`, roda os testes contra um
  Postgres descartável e, se passarem, `docker stack deploy` da stack `xc360-site`.
  `--test-only` só testa.
- Traefik: regra `Host(x360.pistatrack.com) && !PathPrefix(/v1) && !Path(/health) && !PathPrefix(/_bridge)` —
  `/v1/*` e `/health` continuam na ponte `xc360-logs`; `/_bridge/*` (interno) nunca sai pela internet.

## Segredos (nada no repo)
Docker secrets externos: `xc360_site_secret` (cookie), `xc360_site_keyenc` (Fernet, cifra o
TOTP do admin), `xc360_site_db` (DSN role `xc360`), `xc360_site_r2` (R2_ACCOUNT/ACCESS/SECRET/BUCKET),
`xc360_bridge_token` (segredo compartilhado site<->ponte do `/_bridge/check`, também montado na ponte),
`xc360_bridge_db` (DSN só-leitura, legado, não usado). Cópia em
`devbox:~/credentials/xc360-site.env` (chmod 600). Exemplo de formato: `.env.example`.

## Admin
`docker exec $(docker ps -q -f name=xc360-site_web) python -m xc360site.cli admin-link [--hours 72]`
imprime um link de uso único; quem abrir define nome, e-mail, senha e TOTP (obrigatório).

## Chaves beta — CHAVE = UM CONSOLE (contrato com a ponte)
- Formato: `XXXXX-XXXXX`, alfabeto `23456789ABCDEFGHJKMNPQRSTUVWXYZ`.
  `key_sha256 = sha256_hex(UPPER(chave sem '-' e espaços))`. A chave em claro nunca fica no
  banco (a gerada pelo admin fica só cifrada em `pending_enc` até o testador ver uma vez).
- **Banco = fonte da verdade** (`bridgekeys.py`): `beta_keys` ganhou `console_id`, `bound_at`,
  `last_used_at`, `dup_at`, `dup_console`, `dup_ip_h`, `pending_enc`; eventos em `key_events`
  (`bind`, `duplicate`, `unbind`, `new_by_admin`; com os dois console ids e o hash do IP).
- **Ponte -> site**: a ponte (stdlib) chama `POST http://xc360-site_web:8000/_bridge/check` pela
  rede overlay `xc360_internal` (só as duas stacks), header `X-Bridge-Token` (docker secret
  `xc360_bridge_token`), corpo `{"key_sha256","console","ip_h"}` → `{"status": ok|invalid|revoked|duplicate,
  "tester"}`. Sem token certo, ou vindo pelo Traefik (`X-Forwarded-For`), responde 404. Vínculo e
  derrubada numa transação só (`SELECT ... FOR UPDATE`). A ponte guarda cache de 30 s por
  (chave, console); "duplicate" limpa a chave inteira; site fora → vale o último ok por até 6 h.
- Regra: 1º console que usar a chave fica preso; console diferente → `dup_at` (os dois recebem 403
  `duplicate`). Só o admin libera, na página do testador (`/admin/testers/<id>`; alerta na Visão geral
  e contador na sidebar): **gerar nova chave** (a antiga morre; o testador vê a nova uma vez na área)
  ou **desvincular console** (a mesma chave vale pro próximo console). O testador com chave
  derrubada não consegue regenerar (403) e vê o aviso na área.
- Chave "dev" (segredo `xc360_logs_key` da ponte): validada só na ponte, sem vínculo.
- `keys.tsv` e `tools/beta_keys.py new/revoke/push` foram aposentados (o script só faz `check`).

## Versões / promover
O site lê `releases/{stable,beta}.json` e `releases/<ver>/manifest.json` no R2.
Download estável → redireciona pra `/v1/update/<ver>/xenon.elf` (ponte). Beta → o site
serve o ELF do R2 pra quem está logado. "Promover" grava um pedido (`promotion_requests`)
e mostra `python3 tools/release.py promote <ver>` pra rodar no devbox (a chave de
assinatura não sai do devbox); o pedido fecha sozinho quando o `stable.json` muda.

## Logs
Thread de sync (advisory lock) indexa `logs/AAAA-MM-DD/<console>/*.json.gz` em `log_index`
(1ª vez: tudo; depois: ontem+hoje). Números e filtros saem do índice; o lote completo é
lido do R2 ao abrir.

## Jogos (/jogos) — progresso dos testes
`plays.py` lê cada lote de log (no sync e no backfill) e grava um trecho por `[xn] PLAY <ID> (<nome>)` em
`play_sessions` (+ um trecho de continuação quando o lote começa no meio de um jogo): FPS das linhas `STAT`
(sem os ~4 s iniciais de cada sessão/reconexão), quadros pulados, kbps e o desfecho (`ok`, `drop` = queda de vídeo,
`error` = `STATE ERROR` que não seja login, `hang` = `[wdog] TRAVOU/CRASH`, `nostream` = não chegou a rodar, fica fora).
Console e testador só como hash com sal (`kv.play_salt`); a página pública mostra só contagens.
`log_index.play_v` < `plays.PARSER_V` = lote ainda não lido por este parser: o sync relê aos poucos; de uma vez:
`python -m xc360site.cli backfill-plays`. Selo: ver `plays.badge` (limites também descritos na página).
Bugs na página: só jogo (se for um jogo conhecido dos logs), versão, status, data e a `admin_note` (sem e-mail/telefone);
nunca a descrição, fotos, console ou nome. O admin esconde jogo em `/admin/games` e relato no detalhe do relato.

## Testes
`sh web/deploy/deploy.sh --test-only` (pytest no container, Postgres 16 descartável).
