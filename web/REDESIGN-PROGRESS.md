# Redesign da área logada — progresso (NÃO commitado, NÃO deployado)

Backup do `web/` antes de tudo: `devbox:~/scratch/web-backup-antes-redesign/`.
Cópia de trabalho do agente (Windows): `%TEMP%\claude\...\scratchpad\web` — é sincronizada pro devbox com tar.

## Feito
- **Sistema de design** `xc360site/static/css/app.css` (tokens, botões, inputs, toggles, badges, tabelas com
  cabeçalho fixo/zebra/cards no celular, métricas + sparkline, gráficos de barra SVG, empty states, toasts,
  modais `<dialog>`, lightbox, linha do tempo, terminal de log, telas de auth). Só para área logada/auth;
  a home pública continua no `site.css`.
- **JS** `static/js/app.js` (sem dependências): copiar com 1 clique, modal de confirmação (`form[data-confirm]`),
  drawer da sidebar, dropdowns, datas no fuso local (`<time data-local>`), upload arrastar-e-soltar com
  pré-visualização, lightbox, dica dos gráficos, busca/destaque no log, range com saída.
- **Ícones** `xc360site/icons.py` (Lucide, ISC/MIT — falta registrar em `THIRD_PARTY.md`).
- **Helpers** `xc360site/ui.py` (datas por idioma com Babel, `ago`, bandeira emoji, iniciais, paginação,
  geometria de gráficos, `loc()` multilíngue). Filtros Jinja novos em `__init__.py`: `dt` (texto), `date`,
  `when` (`<time>`), `ago`; contexto: `icon`, `flag`, `initials`, `loc`, `nav_counts`, `query_with`, `LANG_NAME`, `asset_v`.
- **Layouts**: `templates/app/_layout.html` (sidebar + topbar + idioma + menu do usuário + modal de confirmação + lightbox),
  `templates/auth/_layout.html` (cartão central com fundo da marca), macros em `templates/_ui.html` e `_report_parts.html`.
- **Telas reescritas**: login, login_2fa, signup (erros por campo, aceite não desmarca), setup, setup_2fa, message, error;
  testador: `area.html` (início), `tester/report_form.html`, `tester/reports.html`, `tester/report_detail.html`;
  admin: overview, testers, invites (modal com prazo 1–15 dias), reports, report_detail (galeria, linha do tempo,
  logs relacionados), logs, log_view (terminal), versions (sem botão Promover duplicado quando há pedido pendente).
- **Views**: `views/admin.py` reescrito (filtros, paginação, contagens, detalhe do relato, eventos de status,
  CRUD de recursos e notas de versão); `views/tester.py` (rotas novas `/area/report`, `/area/reports` GET,
  `/area/reports/<id>`); `views/auth.py` (erros por campo); `views/public.py` (`/status`, `/status/v/<ver>`, home lê "Em breve" do banco).
- **Banco** (`schema.sql`, idempotente): `report_events`, `features`, `release_notes`, `release_note_items`.
  Sementes em `xc360site/seed.py` (rodam 1x por banco, marca `kv.seed_status_v1`).
- Corrigidos os bugs listados pelo coordenador: colunas quebrando/botão cortado (Testadores), tabelas → cards no
  celular, abas cortadas (agora sidebar/drawer), espaçamento, datas ISO cruas, aviso duplicado de promover,
  rótulos desalinhados, aceite desmarcado, upload truncado no celular.

## Feito depois (sessão 2)
- Templates do status público e CRUD: `admin/features.html`, `admin/feature_edit.html`, `admin/notes.html`,
  `admin/note_edit.html`, `public/status.html`, `public/release.html`; seção "Em breve" na home (lê `features`),
  link "Status" no topo (`base.html`), `static/css/status.css`, ajuste do topo no celular em `site.css`.
- Traduções novas em `xc360site/i18n_ui.py` (juntadas em `i18n.M`). "Números" virou "Visão geral".
- Testes novos/ajustados em `tests/test_flows.py` (telas do testador, aceite marcado, telas do admin, datas por idioma,
  botão promover sem duplicar, status público + CRUD + sementes).
- Scripts de demo/prints: `~/scratch/redesign/demo.py` (banco `demo` do pgserver + MemStorage, https://127.0.0.1:8443)
  e `~/scratch/redesign/shots.py` (Playwright) → `~/scratch/site-prints-novo/`.

- Lucide registrado em `THIRD_PARTY.md`. `templates/admin/_base.html` removido.
- Bugs achados nos prints e corrigidos: macro `link` do layout sombreava a variável `link` dos convites (renomeada
  `sb_link`); classe `stack-sm` (utilitário flex) colidia com o modificador de tabela (agora `cards-sm`);
  botões de menu/fechar visíveis no desktop; fundo da sidebar em página longa; pílulas esticadas no /status.

## Falta
- Nada do pedido. Aguardando aprovação do dono pelos prints. Sem deploy, sem commit.

## Como rodar local (devbox)
- venv: `~/scratch/redesign/venv` (requirements + pytest + pgserver + playwright).
- Postgres descartável: `~/scratch/redesign/venv/bin/python ~/scratch/redesign/pg.py` (pgserver em `~/scratch/redesign/pgdata`, bancos `t` e `demo`).
- Testes: `cd web && TEST_DATABASE_URL="postgresql://postgres:@/t?host=/mnt/storage/scratch/redesign/pgdata" ~/scratch/redesign/venv/bin/python -m pytest -q -p no:cacheprovider tests`
- Derrubar no fim: matar o postgres do pgserver e apagar `~/scratch/redesign/pgdata` (feito em 2026-10-25; recriar com `pg.py`).

## Estado dos testes
- Antes do redesign: 31 passaram.
- Depois: 37 passaram (31 originais, 1 ajustado + 6 novos), Postgres local descartável (pgserver).

## Log
