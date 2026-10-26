export interface Env {
  LOGS: R2Bucket;
  API_KEY_V0: string;
}

const MAX_BODY_BYTES = 1_048_576;
const ALLOWED_UA = /^XCloud360\/\d+\.\d+\.\d+/;

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);

    if (req.method === "GET" && url.pathname === "/v1/health") {
      return json({ ok: true, service: "xcloud360-telemetry" });
    }

    if (req.method === "POST" && url.pathname === "/v1/ingest") {
      return handleIngest(req, env);
    }

    return text("not found", 404);
  },
};

async function handleIngest(req: Request, env: Env): Promise<Response> {
  const apiKey = req.headers.get("X-Api-Key");
  if (!apiKey || apiKey !== env.API_KEY_V0) {
    return text("unauthorized", 401);
  }

  const ua = req.headers.get("User-Agent") ?? "";
  if (!ALLOWED_UA.test(ua)) {
    return text("bad user-agent", 400);
  }

  const contentLength = parseInt(req.headers.get("Content-Length") ?? "0", 10);
  if (contentLength <= 0 || contentLength > MAX_BODY_BYTES) {
    return text("bad content-length", 413);
  }

  const buf = await req.arrayBuffer();
  const bodyBytes = new Uint8Array(buf);
  if (bodyBytes.length > MAX_BODY_BYTES) {
    return text("too large", 413);
  }

  const contentEncoding = req.headers.get("Content-Encoding") ?? "";

  let jsonText: string;
  try {
    if (contentEncoding === "gzip") {
      const stream = new Blob([bodyBytes]).stream().pipeThrough(new DecompressionStream("gzip"));
      jsonText = await new Response(stream).text();
    } else {
      jsonText = new TextDecoder().decode(bodyBytes);
    }
  } catch {
    return text("bad body", 400);
  }

  let payload: Record<string, unknown>;
  try {
    payload = JSON.parse(jsonText);
  } catch {
    return text("bad json", 400);
  }

  const consoleId = String(payload.console_id ?? "");
  const sessionId = String(payload.session_id ?? "");
  const appVersion = String(payload.app_version ?? "unknown");
  const game = String(payload.game ?? "unknown");
  const flushReason = String(payload.flush_reason ?? "unknown");

  const safeConsole = consoleId.replace(/[^a-zA-Z0-9_-]/g, "").slice(0, 64);
  const safeSession = sessionId.replace(/[^a-zA-Z0-9_-]/g, "").slice(0, 64);
  if (!safeConsole || !safeSession) {
    return text("bad id format", 400);
  }

  const now = new Date();
  const dateStr = now.toISOString().slice(0, 10);
  const ms = now.getTime();
  const key = `logs/${dateStr}/${safeConsole}/${safeSession}-${ms}.json.gz`;

  const toStore = contentEncoding === "gzip" ? bodyBytes : await gzipEncode(bodyBytes);

  await env.LOGS.put(key, toStore, {
    httpMetadata: {
      contentType: "application/json",
      contentEncoding: "gzip",
    },
    customMetadata: {
      app_version: appVersion,
      game: game.replace(/[^\x20-\x7E]/g, "").slice(0, 128),
      flush_reason: flushReason,
      flushed_at: String(payload.flushed_at ?? now.toISOString()),
    },
  });

  return new Response(null, { status: 204 });
}

async function gzipEncode(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([data]).stream().pipeThrough(new CompressionStream("gzip"));
  const out = await new Response(stream).arrayBuffer();
  return new Uint8Array(out);
}

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function text(body: string, status = 200): Response {
  return new Response(body + "\n", {
    status,
    headers: { "Content-Type": "text/plain" },
  });
}
