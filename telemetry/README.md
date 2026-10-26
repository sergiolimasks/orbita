# XCloud360 Telemetry

Cloudflare Worker that ingests beta telemetry from Xbox 360 consoles into R2.

## Architecture

```
Xbox 360 (XCloud360 app)
   │  POST /v1/ingest   (JSON, gzipped, X-Api-Key header)
   ▼
Cloudflare Worker ──▶ R2 bucket  xcloud360-logs
                       │
                       ▼
                     Python viewer (list.py / pull.py)
```

- `worker/` — Cloudflare Worker (TypeScript)
- `viewer/` — Python CLI to list and fetch logs from R2

## One-time deploy

Prerequisite: `wrangler login` already done.

### 1. Install deps
```bash
cd telemetry/worker
npm install
```

### 2. Create the bucket (if not already created)
```bash
wrangler r2 bucket create xcloud360-logs
```

### 3. Push the API key as a secret
Generated API key for v0.1.x releases:
```
xck_J7vhwYa-lpEK4qgzAViuGdzh3iwCUkPlS4cl3AVJEro
```

Push it to Cloudflare:
```bash
npm run secrets:set
# paste the key when prompted, hit enter
```

### 4. Deploy
```bash
npm run deploy
```

Output will show the live URL, something like:
```
https://xcloud360-telemetry.<your-account>.workers.dev
```

### 5. Smoke test
```bash
curl https://xcloud360-telemetry.<your-account>.workers.dev/v1/health
# expected: {"ok":true,"service":"xcloud360-telemetry"}
```

### 6. (Optional) Set 30-day retention on the bucket
Dashboard → R2 → `xcloud360-logs` → Settings → Object lifecycle rules → Add rule
→ Delete objects after 30 days, prefix `logs/`.

Or via API — see `rclone`/`aws s3api put-bucket-lifecycle-configuration` (R2
speaks the S3 lifecycle API).

## Viewing logs locally

### 1. Create an R2 API token
Dashboard → R2 → **Manage R2 API Tokens** → Create → "Admin Read & Write" on
`xcloud360-logs` → copy Access Key ID + Secret.

### 2. Export credentials
```bash
export R2_ACCOUNT_ID=<CLOUDFLARE_ACCOUNT_ID>
export R2_ACCESS_KEY_ID=...
export R2_SECRET_ACCESS_KEY=...
```

### 3. Install Python deps
```bash
cd telemetry/viewer
pip install -r requirements.txt
```

### 4. List and pull
```bash
# last 24h, all consoles
./list.py

# last 7 days, verbose (shows game + version)
./list.py --days 7 -v

# filter by console_id (prefix match)
./list.py --console abc123

# filter by game
./list.py --days 7 --game ASSASSINS -v

# download one session
./pull.py logs/2026-10-25/<console>/<session>-<ms>.json.gz

# just the events, one JSON per line (grep-friendly)
./pull.py <key> --events | grep '"tag":"ice"'
```

## Rotating the API key

When beta expands to v0.2.0, generate a new key and keep v0.1.0 alive for a few
weeks so old installs don't break:

```bash
# generate
python3 -c "import secrets; print('xck_' + secrets.token_urlsafe(32))"

# update Worker — needs code change to support multiple keys
# (TODO: add API_KEY_V1 secret + check both in handleIngest)
```

For now the Worker accepts a single `API_KEY_V0` secret. When you need two,
edit `worker/src/index.ts` to check both.

## Costs

- Workers: 100k req/day free. At 1 req/console/min, 100k = ~1,666 consoles
  running 1h simultaneously. Beta won't hit this.
- R2: 10GB storage free, zero egress. Each log blob is ~5–50 KB gzipped. 10GB
  = 200k–2M sessions. 30-day retention keeps footprint bounded.
- No credit card needed until you blow past free tier.
