# XCloud360 Telemetry Protocol v1

Spec for the Xbox 360 → telemetry endpoint upload. This is the contract the
app must honor. Worker code lives in `telemetry/worker/`.

## Endpoint

```
POST https://xcloud360-telemetry.<account>.workers.dev/v1/ingest
```

Final URL assigned after `wrangler deploy` — hardcode it into the app's
`telemetry.c` (or wherever the uploader lives).

## Request

### Headers (required)
```
Content-Type:     application/json
Content-Encoding: gzip
User-Agent:       XCloud360/<semver>        e.g. XCloud360/0.1.0-beta
X-Api-Key:        xck_J7vhwYa-lpEK4qgzAViuGdzh3iwCUkPlS4cl3AVJEro
Content-Length:   <bytes of gzipped body>
```

- `User-Agent` MUST match regex `^XCloud360/\d+\.\d+\.\d+` or request is 400'd.
- `X-Api-Key` is the shared beta key. Baked into the binary. Not a secret, but
  filters scanner noise.
- Body MUST be gzip-encoded. Server also accepts non-gzipped as a dev fallback
  but prefer gzipped — saves bandwidth on flaky xCloud sessions.

### Body (JSON)

```json
{
  "schema": 1,
  "console_id": "<128-bit hex, generated once on first boot>",
  "session_id": "<uuid v4, generated per session>",
  "app_version": "0.1.0-beta",
  "build_sha": "a82e193",
  "started_at": "2026-10-25T14:30:00Z",
  "flushed_at": "2026-10-25T14:31:00Z",
  "flush_reason": "periodic | session_end | error | crash",
  "game": "ASSASSINSCREEDODYSSEY",
  "hw": {
    "model": "unknown",
    "net_rtt_ms": 11
  },
  "events": [
    { "t": 123.45, "lvl": "info",  "tag": "ice",   "msg": "candidate pair selected" },
    { "t": 123.50, "lvl": "stat",  "tag": "video", "bitrate_kbps": 1400, "fps": 58.9, "loss_pct": 0.0 },
    { "t": 123.80, "lvl": "warn",  "tag": "input", "msg": "controller 2 lost" },
    { "t": 125.10, "lvl": "error", "tag": "net",   "msg": "connection reset", "errno": 104 }
  ]
}
```

### Field rules

- **`console_id`**: random 128 bits, hex-encoded, generated on first boot,
  stored at `XC360/CONSOLE_ID.TXT` on the pendrive. Deletable (user "forgets").
  Max 64 chars after sanitization (`[a-zA-Z0-9_-]`).
- **`session_id`**: UUID v4, one per app launch. Same session may flush
  multiple times (periodic + session_end). Server stores each flush as a
  separate R2 object.
- **`flush_reason`**: one of `periodic`, `session_end`, `error`, `crash`.
- **`game`**: xCloud title ID or display name. ASCII only, trimmed to 128
  chars.
- **`events[]`**: array of event objects, chronological. Each has `t` (seconds
  since session start, float) and either `msg` (string) or stat fields.
- **Total body (gzipped) MUST be ≤ 1 MB.** Server rejects larger with 413.

### What NOT to send

Hard rules — these would be a privacy/legal problem:

- ❌ xCloud auth token
- ❌ Microsoft account email or gamertag
- ❌ Device login codes (`ABCD-1234` from `microsoft.com/link`)
- ❌ IP addresses (CF strips `CF-Connecting-IP`, don't help it leak)
- ❌ Game session ICE ufrag/pwd (session-scoped secret)
- ❌ Any xCloud server endpoint URL beyond region prefix

Filter aggressively in the uploader before gzip. If in doubt, log it as `msg:
"<redacted>"` with the structural info only.

## Response

| Status | Meaning                                           | Action        |
|--------|---------------------------------------------------|---------------|
| 204    | Accepted, stored                                  | Clear buffer  |
| 400    | Bad body / UA / JSON / IDs                        | Discard batch (bug) |
| 401    | Bad `X-Api-Key`                                   | Discard, don't retry |
| 413    | Too large                                         | Trim buffer, retry smaller |
| 429    | Rate limited (reserved — not yet implemented)     | Backoff 60s   |
| 5xx    | Server down                                       | Buffer & retry (exp backoff: 10s, 30s, 60s, cap 5min) |

**No response body is guaranteed.** Treat anything 2xx as success.

## Upload cadence

- **Periodic flush**: every 60s during a session, `flush_reason: "periodic"`.
- **Session end**: when user exits the game or app, `flush_reason:
  "session_end"`.
- **Error**: on recoverable error (session lost, auth expired),
  `flush_reason: "error"`.
- **Crash**: on crash (if you can — libxenon doesn't give signal handlers for
  everything, best-effort), `flush_reason: "crash"`.

Buffer size: keep last ~200 KB of events in RAM. If network is down, drop
oldest. On reconnect, flush. One in-flight upload at a time (no parallel) to
avoid memory pressure.

## Opt-out

If pendrive has file `XC360/NOTELEMETRY.TXT`, telemetry is OFF.

For beta (v0.1.x – v0.9.x): default **ON**, print message on first boot
("Beta telemetry is enabled — see README to disable").

For v1.0+: flip default to **OFF**, user opts in by creating
`XC360/TELEMETRY.TXT`.

## Dev mode (local UDP fallback)

If pendrive has file `XC360/DEVLOG.TXT` containing a line like `host=<ip>
port=9003`, telemetry ALSO sends line-by-line UDP to that address (for
Sergio's devbox listener). HTTP upload still happens.

If `DEVLOG.TXT` has `only=1` set, HTTP upload is skipped (dev-only mode, no
cloud).

## Reference uploader structure (pseudo-C)

```c
typedef struct {
    char buf[200 * 1024];
    size_t used;
    uint32_t seq;
    uint64_t session_start_ms;
    char session_id[37];
    char console_id[33];
} log_buffer_t;

// Call periodically from main loop
void telemetry_tick(log_buffer_t *lb, const char *reason) {
    uint64_t now_ms = get_time_ms();
    static uint64_t last_flush = 0;

    bool should_flush = false;
    if (strcmp(reason, "periodic") == 0 && now_ms - last_flush > 60000) should_flush = true;
    if (strcmp(reason, "session_end") == 0) should_flush = true;
    if (strcmp(reason, "error") == 0) should_flush = true;

    if (!should_flush || lb->used == 0) return;

    char *json = build_json_envelope(lb, reason);  // see schema above
    size_t gz_len;
    uint8_t *gz = gzip_compress(json, strlen(json), &gz_len);

    int status = http_post(
        TELEMETRY_URL,
        gz, gz_len,
        /* headers */ "Content-Type: application/json\r\n"
                      "Content-Encoding: gzip\r\n"
                      "User-Agent: XCloud360/" APP_VERSION "\r\n"
                      "X-Api-Key: " TELEMETRY_API_KEY "\r\n"
    );

    if (status == 204) {
        lb->used = 0;             // clear buffer
        last_flush = now_ms;
    } else if (status >= 500 || status == 0) {
        // backoff + retry on next tick
    } else {
        // 4xx — drop batch, log one line, don't retry
        lb->used = 0;
    }

    free(json);
    free(gz);
}
```
