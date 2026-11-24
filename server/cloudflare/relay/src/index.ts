// The Compass push relay (README.md). Cloudflare-hosted sync servers
// (server/cloudflare) have no APNs or FCM keys; a server hands each
// device's push token to the relay, which seals it into a handle only the
// relay can open, and later asks the relay to push to that handle. The
// relay sends one of two fixed pushes (a sent tab's alert, a silent sync),
// so a server can't make it show anything else, and stores nothing but
// per-device counts.
//
//   GET  /v1                → {"relay": "compass-push", "version": 1, "apns": bool, "fcm": bool}
//   POST /v1/seal {server, platform, token, environment, topic} → {"handle"}
//   POST /v1/push {server, handle, type: "send-tab" | "sync"}   → {"result": "sent"}
//
// Errors are {"error": code}: 400 bad_request, 400 bad_handle (not ours,
// or for another server: drop it), 403 server_blocked, 410 gone (Apple or
// Google said the device is gone: drop it), 429 rate_limited, 502
// upstream (Apple or Google failed; try later), 503 unavailable.

import { apnsEnabled, apnsGone, sendAPNs, validAPNsToken } from "./apns";
import { deviceName } from "./device";
import { type Env, list, type PushType, type Registration, type Sent } from "./env";
import { fcmEnabled, fcmGone, sendFCM, validFCMToken } from "./fcm";
import { canSeal, open, seal } from "./seal";

export { Device } from "./device";

const RELAY_VERSION = 1;

function json(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body) + "\n", {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

const fail = (status: number, error: string) => json(status, { error });

// validServer: a sync server's host as it gives it (a hostname, maybe with
// a port for tests).
function validServer(s: unknown): s is string {
  return typeof s === "string" && s.length <= 253 && /^[a-z0-9.-]+(:\d{1,5})?$/.test(s);
}

async function body(req: Request): Promise<Record<string, unknown> | null> {
  const len = Number(req.headers.get("content-length") ?? "0");
  if (len > 8192) return null;
  try {
    const text = await req.text();
    if (text.length > 8192) return null;
    const v = JSON.parse(text);
    return v && typeof v === "object" && !Array.isArray(v) ? v : null;
  } catch {
    return null;
  }
}

async function serverGate(env: Env, server: string): Promise<Response | null> {
  if (list(env.BLOCKED_SERVERS).includes(server)) return fail(403, "server_blocked");
  if (env.SERVER_LIMIT && !(await env.SERVER_LIMIT.limit({ key: server })).success) {
    return fail(429, "rate_limited");
  }
  return null;
}

async function handleSeal(req: Request, env: Env): Promise<Response> {
  const b = await body(req);
  if (!b || !validServer(b.server)) return fail(400, "bad_request");
  const server = b.server.toLowerCase();
  const gated = await serverGate(env, server);
  if (gated) return gated;
  if (!(await canSeal(env))) return fail(503, "unavailable");
  const platform = b.platform ?? "apns";
  const topic = typeof b.topic === "string" ? b.topic : "";
  if (!list(env.TOPICS).includes(topic.toLowerCase())) return fail(400, "bad_request");
  let reg: Registration;
  if (platform === "apns") {
    const token = typeof b.token === "string" ? b.token.toLowerCase() : "";
    if (!validAPNsToken(token) || (b.environment !== "sandbox" && b.environment !== "production")) {
      return fail(400, "bad_request");
    }
    if (!apnsEnabled(env)) return fail(503, "unavailable");
    reg = { p: "apns", t: token, e: b.environment, o: topic, h: server, i: Math.floor(Date.now() / 1000) };
  } else if (platform === "fcm") {
    const token = typeof b.token === "string" ? b.token : "";
    if (!validFCMToken(token)) return fail(400, "bad_request");
    if (!fcmEnabled(env)) return fail(503, "unavailable");
    reg = { p: "fcm", t: token, e: "", o: topic, h: server, i: Math.floor(Date.now() / 1000) };
  } else {
    return fail(400, "bad_request");
  }
  return json(200, { handle: await seal(env, reg) });
}

async function handlePush(req: Request, env: Env): Promise<Response> {
  const b = await body(req);
  if (!b || !validServer(b.server) || typeof b.handle !== "string") return fail(400, "bad_request");
  const type = b.type;
  if (type !== "send-tab" && type !== "sync") return fail(400, "bad_request");
  const server = b.server.toLowerCase();
  const gated = await serverGate(env, server);
  if (gated) return gated;
  const reg = await open(env, b.handle);
  if (!reg || reg.h !== server) return fail(400, "bad_handle");
  // Android gets no background push (fcm.ts).
  if (reg.p === "fcm" && type === "sync") return json(200, { result: "not_sent" });
  const device = env.DEVICE.get(env.DEVICE.idFromName(await deviceName(reg.p, reg.t)));
  const admit = await device.admit(type as PushType);
  if (admit === "gone") return fail(410, "gone");
  if (admit === "limited") return fail(429, "rate_limited");
  const sent: Sent = reg.p === "apns" ? await sendAPNs(env, reg, type) : await sendFCM(env, reg);
  if (reg.p === "apns" ? apnsGone(sent) : fcmGone(sent)) {
    await device.gone();
    return fail(410, "gone");
  }
  if (sent.status !== 200) {
    console.warn(`push refused: ${reg.p} ${sent.status} ${sent.reason}`);
    return json(502, { error: "upstream", status: sent.status, reason: sent.reason });
  }
  return json(200, { result: "sent" });
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const path = new URL(req.url).pathname.replace(/\/+$/, "");
    if (path === "/v1" || path === "") {
      if (req.method !== "GET") return fail(405, "method_not_allowed");
      return json(200, { relay: "compass-push", version: RELAY_VERSION, apns: apnsEnabled(env), fcm: fcmEnabled(env) });
    }
    if (path !== "/v1/seal" && path !== "/v1/push") return fail(404, "not_found");
    if (req.method !== "POST") return fail(405, "method_not_allowed");
    return path === "/v1/seal" ? handleSeal(req, env) : handlePush(req, env);
  },
} satisfies ExportedHandler<Env>;
