// The Compass sync server on Cloudflare Workers: the Go server's HTTP API
// (docs/design/sync.md, "Server") on Durable Objects. The Worker checks
// the sync token and who may use the server, then hands the request to the
// chain's object (chain.ts), the pairing relay (pairing.ts), the access
// list (access.ts) or, for rooms, rooms.ts and the room's object (room.ts).
//
// Not here yet: push (sent tabs and background sync reach devices at their
// next sync instead) and company sign-in.

import { type AccessState } from "./access";
import { AuthError, bearer, idList, verifyToken } from "./auth";
import { type Env, fail, json, text } from "./config";
import { SERVER_VERSION } from "./version";
import { forgetGate, rooms } from "./rooms";

export { Access } from "./access";
export { Chain } from "./chain";
export { Pairing } from "./pairing";
export { Room } from "./room";

const LINKED_TOKEN_HEADER = "X-Compass-Linked-Token";
const KEEP_ALIVE_MS = 25_000;
const ACCESS_TTL_MS = 60_000;
const VERSION = `cloudflare-${SERVER_VERSION}`;

// Access answers, cached per isolate for a minute: a removed friend is
// refused within that.
const accessCache = new Map<string, { state: AccessState; until: number }>();

function access(env: Env) {
  return env.ACCESS.get(env.ACCESS.idFromName("access"));
}

async function accessState(env: Env, chain: string): Promise<AccessState> {
  const key = chain.toLowerCase();
  const now = Date.now();
  const hit = accessCache.get(key);
  if (hit && hit.until > now) return hit.state;
  const state = await access(env).state(key);
  if (accessCache.size > 10000) accessCache.clear();
  accessCache.set(key, { state, until: now + (state.allowed ? ACCESS_TTL_MS : 10_000) });
  return state;
}

// authenticate returns the chain of a valid, unblocked token, or "".
async function authenticate(env: Env, header: string | null): Promise<string> {
  const token = bearer(header);
  if (!token) return "";
  try {
    const id = await verifyToken(token);
    return idList(env.BLOCKED_CLIENT_IDS).includes(id.toLowerCase()) ? "" : id;
  } catch (e) {
    if (!(e instanceof AuthError)) console.error("token check failed", e);
    return "";
  }
}

// authorize returns the chain of a request whose token is valid and whose
// chain may use the server, or "".
async function authorize(env: Env, request: Request): Promise<string> {
  const chain = await authenticate(env, request.headers.get("Authorization"));
  if (!chain) return "";
  return (await accessState(env, chain)).allowed ? chain : "";
}

function chainStub(env: Env, chain: string) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}

function toChain(env: Env, chain: string, path: string, request: Request, init: RequestInit = {}): Promise<Response> {
  const url = new URL(request.url);
  const headers = new Headers(init.headers);
  headers.set("X-Chain-ID", chain);
  const enc = request.headers.get("Content-Encoding");
  if (enc) headers.set("Content-Encoding", enc);
  return chainStub(env, chain).fetch(
    new Request(`https://chain${path}${url.search}`, {
      method: request.method,
      body: request.method === "GET" || request.method === "HEAD" ? null : request.body,
      ...init,
      headers,
    }),
  );
}

export default {
  async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;
    const method = request.method;

    if (path === "/" && method === "GET") return new Response(".", { status: 200 });
    if (path === "/healthz") return text(200, "ok");
    if (path === "/health-check") return json(200, { status: "ok", version: VERSION, commit: VERSION });
    if (path === "/invite" && method === "GET") return invitePage();
    if (!path.startsWith("/v2/")) return text(404, "404 page not found");
    const rest = path.slice(3); // "/command/", "/compass/..."

    // The sync protocol.
    if (rest === "/command/") {
      if (method !== "POST") return text(405, "Method Not Allowed");
      const chain = await authorize(env, request);
      if (!chain) return text(401, "Unauthorized");
      const rsp = await toChain(env, chain, "/command/", request);
      forgetGate(chain); // It may have reset the chain.
      return rsp;
    }

    // Which server this is, for Compass's update offer (version.ts).
    if (rest === "/compass/info" && method === "GET") {
      return json(200, { login: null, server: { kind: "cloudflare", version: SERVER_VERSION } });
    }

    // Rooms: a chain the access list doesn't name may still use the rooms
    // it's in, so these routes check the list themselves.
    const r = await rooms(env, request, rest, {
      authenticate,
      listed: async (e, chain) => (await accessState(e, chain)).allowed,
    });
    if (r) return r;

    if (rest === "/compass/events") {
      if (method !== "GET") return fail(405, "GET only", { Allow: "GET" });
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      return events(env, chain, request, ctx);
    }

    if (rest === "/compass/quota") {
      if (method !== "GET") return text(405, "method not allowed");
      const chain = await authorize(env, request);
      if (!chain) return text(401, "Unauthorized");
      return toChain(env, chain, "/quota", request);
    }

    if (rest === "/compass/devices" || rest.startsWith("/compass/devices/")) {
      if (method !== "DELETE") return text(405, "Method Not Allowed");
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      const guid = rest.slice("/compass/devices".length); // "" or "/<escaped guid>"
      const rsp = await toChain(env, chain, "/devices" + guid, request);
      forgetGate(chain);
      return rsp;
    }

    if (rest === "/compass/push") {
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      // No APNs or FCM keys on a self-hosted Worker: registrations are
      // accepted and nothing is pushed; devices learn of sent tabs at their
      // next sync, as on a Go server without keys.
      if (method === "POST" || method === "DELETE") {
        await request.body?.cancel();
        return new Response(null, { status: 204 });
      }
      return text(405, "method not allowed");
    }

    if (rest === "/compass/pair" || rest.startsWith("/compass/pair/")) {
      const headers = new Headers(request.headers);
      headers.delete("X-Chain-ID");
      if (request.headers.get("Authorization")) {
        const chain = await authorize(env, request);
        if (chain) headers.set("X-Chain-ID", chain);
      }
      headers.set("X-Client-IP", request.headers.get("CF-Connecting-IP") ?? "");
      headers.set("X-Host", url.host);
      const stub = env.PAIRING.get(env.PAIRING.idFromName("pairing"));
      return stub.fetch(
        new Request(`https://pairing${rest.slice("/compass/pair".length) || "/"}${url.search}`, {
          method,
          headers,
          body: method === "GET" || method === "DELETE" ? null : request.body,
          signal: request.signal,
        }),
      );
    }

    // Profile accounts: link another chain to the caller's (sync.md,
    // "Profiles"); the linked chain passes the access list with it.
    if (rest === "/compass/account/link" && method === "POST") {
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      // The linked token comes with or without "Bearer " (as in Go).
      const raw = (request.headers.get(LINKED_TOKEN_HEADER) ?? "").trim();
      const other = raw ? await authenticate(env, raw.startsWith("Bearer ") ? raw : "Bearer " + raw) : "";
      if (!other) return fail(400, "invalid " + LINKED_TOKEN_HEADER);
      accessCache.delete(other.toLowerCase());
      return access(env).link(chain, other);
    }

    // The access list: owners invite friends, friends join with a code.
    if (rest === "/compass/access/join" && method === "POST") {
      const chain = await authenticate(env, request.headers.get("Authorization"));
      if (!chain) return fail(401, "invalid sync token");
      const body = await readJSON(request);
      if (typeof body?.code !== "string" || !body.code) return fail(400, "bad_request");
      accessCache.delete(chain.toLowerCase());
      return access(env).join(chain, body.code.trim().toLowerCase());
    }
    if (rest === "/compass/access" || rest.startsWith("/compass/access/")) {
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      if (rest === "/compass/access" && method === "GET") return access(env).members(chain);
      if (rest === "/compass/access/invites" && method === "POST") {
        const body = await readJSON(request);
        const name = typeof body?.name === "string" ? body.name : "";
        return access(env).createInvite(chain, name, url.origin);
      }
      const target = decodeURIComponent(rest.slice("/compass/access/".length));
      if (method === "DELETE" && target) {
        accessCache.delete(target.toLowerCase());
        return access(env).remove(chain, target);
      }
      return fail(404, "not found");
    }

    return text(404, "404 page not found");
  },
} satisfies ExportedHandler<Env>;

async function readJSON(request: Request): Promise<Record<string, unknown> | null> {
  try {
    const v = await request.json();
    return v && typeof v === "object" ? (v as Record<string, unknown>) : null;
  } catch {
    return null;
  }
}

// events serves GET /v2/compass/events, the change stream (sync.md,
// "Change notifications"), as the Go server does: ": connected", a
// keep-alive comment every 25 s, and "event: changed" when another device
// commits. The chain's object announces changes on a hibernating WebSocket,
// so it costs nothing while the chain is quiet; this Worker holds the
// client's stream and writes the keep-alives.
async function events(env: Env, chain: string, request: Request, ctx: ExecutionContext): Promise<Response> {
  const rsp = await toChain(env, chain, "/events", request, {
    method: "GET",
    headers: { Upgrade: "websocket" },
    body: null,
  });
  const ws = rsp.webSocket;
  if (!ws) return rsp; // The object's refusal: 403, 429, ...
  ws.accept();

  const { readable, writable } = new TransformStream<Uint8Array, Uint8Array>();
  const writer = writable.getWriter();
  const enc = new TextEncoder();
  let done = false;
  let resolveDone: () => void = () => {};
  const finished = new Promise<void>((r) => (resolveDone = r));
  const close = () => {
    if (done) return;
    done = true;
    clearInterval(timer);
    try {
      ws.close(1000, "stream closed");
    } catch {
      // Already closed.
    }
    writer.close().catch(() => {});
    resolveDone();
  };
  const send = (s: string) => {
    if (!done) writer.write(enc.encode(s)).catch(close);
  };
  const timer = setInterval(() => send(": keep-alive\n\n"), KEEP_ALIVE_MS);
  ws.addEventListener("message", (ev) => {
    if (typeof ev.data === "string") send(ev.data);
  });
  ws.addEventListener("close", close);
  ws.addEventListener("error", close);
  request.signal?.addEventListener?.("abort", close);
  send(": connected\n\n");
  ctx.waitUntil(finished);

  return new Response(readable, {
    status: 200,
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-store",
      "X-Accel-Buffering": "no",
    },
  });
}

// invitePage is what an invite link opens in a browser: how to use it.
// The code stays in the fragment; the page reads nothing from it.
function invitePage(): Response {
  const html = `<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Compass sync invite</title>
<style>
  :root { color-scheme: light dark; --bg: #fafaf9; --fg: #1c1917; --muted: #57534e; --card: #fff; --line: #e7e5e4; }
  @media (prefers-color-scheme: dark) { :root { --bg: #1c1917; --fg: #f5f5f4; --muted: #a8a29e; --card: #292524; --line: #44403c; } }
  body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
  main { max-width: 34rem; margin: 12vh auto; padding: 0 16px; }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 12px; padding: 24px; }
  h1 { font-size: 1.4rem; margin: 0 0 8px; }
  p, li { color: var(--muted); }
  ol { padding-left: 1.2rem; }
  code { word-break: break-all; }
</style>
<main><div class="card">
  <h1>You're invited to sync with Compass</h1>
  <p>Someone shared their Compass sync server with you. Your data is
    encrypted on your devices; the server, and its owner, can't read it.</p>
  <ol>
    <li>Open Compass, then Settings, then Sync.</li>
    <li>Choose <b>I have an invite</b>.</li>
    <li>Paste this page's address: <code id="link"></code></li>
  </ol>
  <p>The invite works once and expires after 7 days.</p>
</div></main>
<script>document.getElementById("link").textContent = location.href;</script>
</html>`;
  return new Response(html, {
    status: 200,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "no-store",
      "Referrer-Policy": "no-referrer",
      "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'",
    },
  });
}
