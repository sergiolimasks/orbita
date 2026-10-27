-- XCloud360 site da beta. Idempotente: roda a cada start do app.

CREATE TABLE IF NOT EXISTS users (
    id            bigserial PRIMARY KEY,
    role          text NOT NULL CHECK (role IN ('admin', 'tester')),
    name          text NOT NULL,
    email         text NOT NULL UNIQUE,
    phone         text NOT NULL DEFAULT '',
    country       text NOT NULL DEFAULT '',
    handle        text NOT NULL UNIQUE,          -- tester_name visto pela ponte / nos logs
    pw_hash       text,
    totp_enc      text,                          -- segredo TOTP cifrado (Fernet)
    totp_enabled  boolean NOT NULL DEFAULT false,
    totp_last_step bigint NOT NULL DEFAULT 0,
    active        boolean NOT NULL DEFAULT true,
    failed_logins integer NOT NULL DEFAULT 0,
    locked_until  timestamptz,
    created_at    timestamptz NOT NULL DEFAULT now(),
    last_login_at timestamptz
);

CREATE TABLE IF NOT EXISTS invites (
    id           bigserial PRIMARY KEY,
    token_sha256 text NOT NULL UNIQUE,
    note         text NOT NULL DEFAULT '',
    created_by   bigint REFERENCES users(id),
    created_at   timestamptz NOT NULL DEFAULT now(),
    expires_at   timestamptz NOT NULL,
    used_at      timestamptz,
    used_by      bigint REFERENCES users(id),
    revoked_at   timestamptz
);

CREATE TABLE IF NOT EXISTS setup_links (
    token_sha256 text PRIMARY KEY,
    created_at   timestamptz NOT NULL DEFAULT now(),
    expires_at   timestamptz NOT NULL,
    used_at      timestamptz,
    user_id      bigint REFERENCES users(id)     -- admin em configuração (inativo até o 2FA)
);

CREATE TABLE IF NOT EXISTS sessions (
    id_sha256  text PRIMARY KEY,
    user_id    bigint NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at timestamptz NOT NULL DEFAULT now(),
    last_seen  timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    ip         text NOT NULL DEFAULT '',
    ua         text NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);

CREATE TABLE IF NOT EXISTS login_attempts (
    id    bigserial PRIMARY KEY,
    ip    text NOT NULL,
    kind  text NOT NULL,
    ok    boolean NOT NULL,
    at    timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS login_attempts_ip ON login_attempts(ip, kind, at);

CREATE TABLE IF NOT EXISTS beta_keys (
    id         bigserial PRIMARY KEY,
    user_id    bigint NOT NULL REFERENCES users(id),
    key_sha256 text NOT NULL UNIQUE,   -- sha256 hex da chave normalizada (ver README)
    key_hint   text NOT NULL DEFAULT '',  -- só os 2 últimos caracteres (a chave em claro NÃO fica no banco)
    created_at timestamptz NOT NULL DEFAULT now(),
    revoked_at timestamptz
);
CREATE INDEX IF NOT EXISTS beta_keys_user ON beta_keys(user_id);
-- CHAVE = UM CONSOLE (ver bridgekeys.py): vínculo no 1º uso; 2º console diferente derruba (dup_*)
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS console_id   text;          -- X-Console (32 hex) do 1º console
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS bound_at     timestamptz;
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS last_used_at timestamptz;
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS dup_at       timestamptz;   -- derrubada (só o admin libera)
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS dup_console  text;          -- o 2º console
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS dup_ip_h     text;          -- hash do IP do 2º uso (não o IP)
ALTER TABLE beta_keys ADD COLUMN IF NOT EXISTS pending_enc  text;          -- chave gerada pelo admin, cifrada, até o testador ver 1x

CREATE TABLE IF NOT EXISTS key_events (
    id        bigserial PRIMARY KEY,
    key_id    bigint REFERENCES beta_keys(id) ON DELETE CASCADE,
    user_id   bigint NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind      text NOT NULL CHECK (kind IN ('bind', 'duplicate', 'unbind', 'new_by_admin')),
    console_a text NOT NULL DEFAULT '',
    console_b text NOT NULL DEFAULT '',
    ip_h      text NOT NULL DEFAULT '',
    note      text NOT NULL DEFAULT '',
    by_user   bigint REFERENCES users(id) ON DELETE SET NULL,
    at        timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS key_events_user ON key_events(user_id, at DESC);

-- Legado: a ponte hoje valida via POST /_bridge/check do site (views/bridge.py), não lê o banco.
-- A view (e o role xc360_bridge, só leitura) ficam pra consulta.
CREATE OR REPLACE VIEW bridge_beta_keys AS
    SELECT k.key_sha256,
           u.handle AS tester_name,
           (k.revoked_at IS NULL AND u.active) AS active
      FROM beta_keys k JOIN users u ON u.id = k.user_id;

CREATE TABLE IF NOT EXISTS reports (
    id          bigserial PRIMARY KEY,
    user_id     bigint NOT NULL REFERENCES users(id),
    version     text NOT NULL DEFAULT '',
    console_id  text NOT NULL DEFAULT '',
    game        text NOT NULL DEFAULT '',
    description text NOT NULL,
    status      text NOT NULL DEFAULT 'new',
    admin_note  text NOT NULL DEFAULT '',
    created_at  timestamptz NOT NULL DEFAULT now(),
    updated_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS reports_user ON reports(user_id);

CREATE TABLE IF NOT EXISTS report_files (
    id        bigserial PRIMARY KEY,
    report_id bigint NOT NULL REFERENCES reports(id) ON DELETE CASCADE,
    r2_key    text NOT NULL,
    thumb_key text NOT NULL,
    size      integer NOT NULL,
    width     integer NOT NULL,
    height    integer NOT NULL
);

CREATE TABLE IF NOT EXISTS log_index (
    r2_key  text PRIMARY KEY,
    day     date NOT NULL,
    console text NOT NULL,
    tester  text NOT NULL DEFAULT '',
    app     text NOT NULL DEFAULT '',
    game    text NOT NULL DEFAULT '',
    rx      timestamptz,
    nlines  integer NOT NULL DEFAULT 0,
    size    integer NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS log_index_day ON log_index(day DESC);
CREATE INDEX IF NOT EXISTS log_index_console ON log_index(console);
CREATE INDEX IF NOT EXISTS log_index_tester ON log_index(tester);

CREATE TABLE IF NOT EXISTS kv (k text PRIMARY KEY, v text NOT NULL);

CREATE TABLE IF NOT EXISTS promotion_requests (
    id           bigserial PRIMARY KEY,
    version      text NOT NULL,
    requested_by bigint REFERENCES users(id),
    requested_at timestamptz NOT NULL DEFAULT now(),
    done_at      timestamptz
);

CREATE TABLE IF NOT EXISTS audit (
    id      bigserial PRIMARY KEY,
    user_id bigint REFERENCES users(id),
    action  text NOT NULL,
    detail  text NOT NULL DEFAULT '',
    at      timestamptz NOT NULL DEFAULT now()
);

-- linha do tempo dos relatos (mudanças de status / notas feitas no painel)
CREATE TABLE IF NOT EXISTS report_events (
    id        bigserial PRIMARY KEY,
    report_id bigint NOT NULL REFERENCES reports(id) ON DELETE CASCADE,
    user_id   bigint REFERENCES users(id),
    status    text NOT NULL,
    note      text NOT NULL DEFAULT '',
    at        timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS report_events_report ON report_events(report_id, at);

-- painel público de status (/status): recursos do app e notas de versão, editáveis no admin
CREATE TABLE IF NOT EXISTS features (
    id           bigserial PRIMARY KEY,
    grp          text NOT NULL CHECK (grp IN ('controls', 'video', 'menu', 'system')),
    status       text NOT NULL CHECK (status IN ('working', 'beta', 'tested', 'soon', 'unsupported')),
    name_pt      text NOT NULL,
    name_en      text NOT NULL DEFAULT '',
    name_es      text NOT NULL DEFAULT '',
    desc_pt      text NOT NULL DEFAULT '',
    desc_en      text NOT NULL DEFAULT '',
    desc_es      text NOT NULL DEFAULT '',
    since_version text NOT NULL DEFAULT '',   -- "desde a versão X" (estável) / versão beta onde entrou
    sort         integer NOT NULL DEFAULT 100,
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS features_grp ON features(grp, sort, id);

CREATE TABLE IF NOT EXISTS release_notes (
    id          bigserial PRIMARY KEY,
    version     text NOT NULL UNIQUE,
    channel     text NOT NULL CHECK (channel IN ('beta', 'stable')),
    released_on date,
    summary_pt  text NOT NULL DEFAULT '',
    summary_en  text NOT NULL DEFAULT '',
    summary_es  text NOT NULL DEFAULT '',
    published   boolean NOT NULL DEFAULT false,   -- rascunho até publicar
    created_at  timestamptz NOT NULL DEFAULT now(),
    updated_at  timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS release_note_items (
    id         bigserial PRIMARY KEY,
    note_id    bigint NOT NULL REFERENCES release_notes(id) ON DELETE CASCADE,
    kind       text NOT NULL CHECK (kind IN ('new', 'improved', 'fixed', 'known')),
    text_pt    text NOT NULL,
    text_en    text NOT NULL DEFAULT '',
    text_es    text NOT NULL DEFAULT '',
    feature_id bigint REFERENCES features(id) ON DELETE SET NULL,
    sort       integer NOT NULL DEFAULT 100
);
CREATE INDEX IF NOT EXISTS release_note_items_note ON release_note_items(note_id, sort, id);

-- página pública /jogos: sessões de jogo tiradas dos lotes de log (plays.py). Console e testador só como hash.
ALTER TABLE log_index ADD COLUMN IF NOT EXISTS play_v smallint NOT NULL DEFAULT 0;   -- versão do parser que já leu o lote
CREATE INDEX IF NOT EXISTS log_index_play_v ON log_index(play_v);
CREATE TABLE IF NOT EXISTS play_sessions (
    r2_key    text NOT NULL REFERENCES log_index(r2_key) ON DELETE CASCADE,
    seq       smallint NOT NULL,                 -- trecho dentro do lote (um por "[xn] PLAY")
    day       date NOT NULL,
    rx        timestamptz,
    app       text NOT NULL DEFAULT '',
    console_h text NOT NULL DEFAULT '',          -- hash com sal (kv.play_salt), nunca o id
    tester_h  text NOT NULL DEFAULT '',          -- hash do testador (ou do console se não tiver)
    title_id  text NOT NULL DEFAULT '',
    game      text NOT NULL,
    continued boolean NOT NULL DEFAULT false,    -- lote começou no meio de um jogo (não conta como sessão nova)
    outcome   text NOT NULL CHECK (outcome IN ('ok', 'drop', 'error', 'hang', 'nostream')),
    n_drops   integer NOT NULL DEFAULT 0,
    n_errors  integer NOT NULL DEFAULT 0,
    n_hangs   integer NOT NULL DEFAULT 0,
    samples   integer NOT NULL DEFAULT 0,        -- linhas STAT usadas (2 s cada)
    ge55      integer NOT NULL DEFAULT 0,
    fps_avg   real,
    fps_min   real,
    fps_med   real,
    hist      text NOT NULL DEFAULT '{}',        -- {"59": n, ...} fps arredondado -> amostras
    skipped   integer NOT NULL DEFAULT 0,
    kbps_avg  real,
    loss_avg  real,
    PRIMARY KEY (r2_key, seq)
);
CREATE INDEX IF NOT EXISTS play_sessions_game ON play_sessions(title_id, game);
CREATE TABLE IF NOT EXISTS game_flags (
    gkey       text PRIMARY KEY,                 -- title id (ou nome em minúsculas)
    hidden     boolean NOT NULL DEFAULT false,
    updated_at timestamptz NOT NULL DEFAULT now()
);
ALTER TABLE reports ADD COLUMN IF NOT EXISTS public_hidden boolean NOT NULL DEFAULT false;

-- v2 do parser: motivo de fim mandado pelo servidor (KickFor...)
ALTER TABLE play_sessions ADD COLUMN IF NOT EXISTS kick text NOT NULL DEFAULT '';
