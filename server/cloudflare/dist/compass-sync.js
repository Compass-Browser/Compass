var __defProp = Object.defineProperty;
var __name = (target, value) => __defProp(target, "name", { value, configurable: true });

// src/auth.ts
var TOKEN_MAX_AGE_MS = 86400 * 1e3;
var TOKEN_RE = /^([a-fA-F0-9]+)\|([a-fA-F0-9]+)\|([a-fA-F0-9]+)$/;
var AuthError = class extends Error {
  static {
    __name(this, "AuthError");
  }
};
function hexBytes(hex) {
  if (hex.length % 2) throw new AuthError("odd hex length");
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}
__name(hexBytes, "hexBytes");
function base64UrlDecode(s) {
  const b642 = s.replace(/-/g, "+").replace(/_/g, "/");
  try {
    return atob(b642);
  } catch {
    throw new AuthError("bad base64");
  }
}
__name(base64UrlDecode, "base64UrlDecode");
async function verifyToken(token, now = Date.now()) {
  const m = TOKEN_RE.exec(base64UrlDecode(token));
  if (!m) throw new AuthError("invalid token format");
  const [, tsHex, sigHex, keyHex] = m;
  const key = hexBytes(keyHex);
  const ts = hexBytes(tsHex);
  const sig = hexBytes(sigHex);
  if (key.length !== 32 || sig.length !== 64) throw new AuthError("bad key or signature length");
  let ok = false;
  try {
    const pub = await crypto.subtle.importKey("raw", key, { name: "Ed25519" }, false, ["verify"]);
    ok = await crypto.subtle.verify({ name: "Ed25519" }, pub, sig, ts);
  } catch {
    ok = false;
  }
  if (!ok) throw new AuthError("signature verification failed");
  const tsText = new TextDecoder().decode(ts);
  if (!/^-?\d+$/.test(tsText)) throw new AuthError("bad timestamp");
  if (Math.abs(now - Number(tsText)) > TOKEN_MAX_AGE_MS) throw new AuthError("token is expired");
  return keyHex;
}
__name(verifyToken, "verifyToken");
function bearer(header) {
  if (!header) return "";
  const t = header.trim();
  return t.startsWith("Bearer ") ? t.slice(7).trim() : "";
}
__name(bearer, "bearer");
function idList(v) {
  return (v ?? "").split(",").map((s) => s.trim().toLowerCase()).filter(Boolean);
}
__name(idList, "idList");

// src/config.ts
var DEFAULT_PUSH_RELAY = "https://push.compassbrowser.org";
function num(v, def, ok) {
  if (v === void 0 || v.trim() === "") return def;
  const n = Number(v);
  return Number.isInteger(n) && ok(n) ? n : def;
}
__name(num, "num");
function loadConfig(env) {
  const maxEntities = num(env.COMPASS_SYNC_MAX_ENTITIES, 1e5, (n) => n > 0);
  return {
    maxEntities,
    maxBytes: num(env.COMPASS_SYNC_MAX_BYTES, 200 * 1024 * 1024, (n) => n >= 0),
    // The history quota can't be larger than the whole.
    maxHistory: Math.min(num(env.COMPASS_SYNC_MAX_HISTORY, 3e4, (n) => n > 0), maxEntities),
    historyDays: num(env.COMPASS_SYNC_HISTORY_DAYS, 14, (n) => n >= 1 && n <= 30),
    tombstoneDays: num(env.COMPASS_SYNC_TOMBSTONE_DAYS, 30, (n) => n >= 0),
    idleChainDays: num(env.COMPASS_SYNC_IDLE_CHAIN_DAYS, 180, (n) => n >= 0),
    pollSeconds: num(env.COMPASS_SYNC_POLL_SECONDS, 900, (n) => n >= 30 && n <= 86400),
    highDeviceLimit: new Set(
      (env.HIGH_DEVICE_LIMIT_CLIENT_IDS ?? "").split(",").map((s) => s.trim().toLowerCase()).filter(Boolean)
    ),
    pushRelay: pushRelay(env.COMPASS_SYNC_PUSH_RELAY),
    backgroundIntervalMs: num(env.COMPASS_SYNC_PUSH_BACKGROUND_SECONDS, 900, (n) => n >= 1) * 1e3,
    pushTopics: (env.COMPASS_SYNC_APNS_TOPICS || "it.cipi1965.compass").split(",").map((s) => s.trim()).filter(Boolean)
  };
}
__name(loadConfig, "loadConfig");
function pushRelay(v) {
  if (v === void 0) return DEFAULT_PUSH_RELAY;
  const t = v.trim().replace(/\/+$/, "");
  return t === "" || t === "off" || !/^https?:\/\//.test(t) ? "" : t;
}
__name(pushRelay, "pushRelay");
function json(status, body, headers = {}) {
  return new Response(JSON.stringify(body) + "\n", {
    status,
    headers: { "Content-Type": "application/json", "Cache-Control": "no-store", ...headers }
  });
}
__name(json, "json");
function fail(status, error, headers = {}) {
  return json(status, { error }, headers);
}
__name(fail, "fail");
function text(status, body) {
  return new Response(body + "\n", {
    status,
    headers: { "Content-Type": "text/plain; charset=utf-8", "X-Content-Type-Options": "nosniff" }
  });
}
__name(text, "text");
function deviceRemoved() {
  return fail(403, "device_removed", { "X-Compass-Device": "removed" });
}
__name(deviceRemoved, "deviceRemoved");

// src/version.ts
var SERVER_VERSION = 3;

// src/roomwire.ts
var MAX_WAIT_SECONDS = 30;
var MAX_RECORDS_PER_WRITE = 500;
var MAX_CHANGES_LIMIT = 1e3;
var MAX_WRITE_BYTES = 24 << 20;
var MAX_BATCH_CIPHERTEXT = 16 << 20;
var MAX_BODY = 1 << 20;
var ENVELOPE_BYTES = 80;
var RECORD_OVERHEAD = 28;
var MAX_PROFILE_BYTES = 4096;
var MAX_INVITE_USES = 1e3;
var IDENTITY_KEY_BYTES = 32;
var SECRET_BYTES = 32;
var ROOM_ID_BYTES = 16;
var MAX_ROOM_MEMBERS = 50;
var MAX_ROOM_RECORD_BYTES = 1 << 20;
var MAX_ROOM_INVITES = 100;
var MAX_ROOMS_PER_OWNER = 1e3;
var MAX_CHANGES_BYTES = 8 << 20;
var MAX_ENVELOPES = 1e3;
var RECORD_ID_RE = /^[A-Za-z0-9._:-]{1,128}$/;
var ROOM_ID_RE = /^[A-Za-z0-9_-]{22}$/;
var INVITE_ID_RE = /^[A-Za-z0-9_-]{1,64}$/;
var SECRET_HASH_RE = /^[0-9a-f]{64}$/;
var CHAIN_ID_RE = /^[0-9a-f]{64}$/;
var ROLE_OWNER = "owner";
var ROLE_EDITOR = "editor";
var ROLE_VIEWER = "viewer";
var KINDS = ["space", "folder", "easel"];
var LINKED_TOKEN_HEADER = "X-Compass-Linked-Token";
function b64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 32768) {
    s += String.fromCharCode(...bytes.subarray(i, i + 32768));
  }
  return btoa(s);
}
__name(b64, "b64");
function fromBinary(s) {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}
__name(fromBinary, "fromBinary");
function decodeStd(s) {
  const t = s.replace(/[\r\n]/g, "");
  if (t.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(t)) return null;
  try {
    return fromBinary(atob(t));
  } catch {
    return null;
  }
}
__name(decodeStd, "decodeStd");
function rawURL(bytes) {
  return b64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
__name(rawURL, "rawURL");
function decodeRawURL(s, n) {
  if (!/^[A-Za-z0-9_-]*$/.test(s)) return null;
  let std = s.replace(/-/g, "+").replace(/_/g, "/");
  while (std.length % 4) std += "=";
  let out;
  try {
    out = fromBinary(atob(std));
  } catch {
    return null;
  }
  if (out.length !== n || rawURL(out) !== s) return null;
  return out;
}
__name(decodeRawURL, "decodeRawURL");
function validRoomID(id) {
  return ROOM_ID_RE.test(id) && decodeRawURL(id, ROOM_ID_BYTES) !== null;
}
__name(validRoomID, "validRoomID");
function newID() {
  return rawURL(crypto.getRandomValues(new Uint8Array(16)));
}
__name(newID, "newID");
async function sha256(b) {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", b));
}
__name(sha256, "sha256");
function equalBytes(a, b) {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a[i] ^ b[i];
  return d === 0;
}
__name(equalBytes, "equalBytes");
var HttpError = class extends Error {
  constructor(status, code, extra = {}) {
    super(code);
    this.status = status;
    this.code = code;
    this.extra = extra;
  }
  status;
  code;
  extra;
  static {
    __name(this, "HttpError");
  }
};
var Bad = class extends Error {
  static {
    __name(this, "Bad");
  }
};
function badRequest(detail) {
  return json(400, { error: "bad_request", detail });
}
__name(badRequest, "badRequest");
function noContent() {
  return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
}
__name(noContent, "noContent");
function errorResponse(e) {
  if (e instanceof HttpError) return json(e.status, { error: e.code, ...e.extra });
  if (e instanceof Bad) return badRequest(e.message);
  console.error("rooms: request failed", e);
  return json(500, { error: "unable to complete request" });
}
__name(errorResponse, "errorResponse");
async function readJSONBody(request, limit) {
  const parts = [];
  let size = 0;
  if (request.body) {
    const reader = request.body.getReader();
    for (; ; ) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > limit) {
        await reader.cancel().catch(() => {
        });
        throw new HttpError(413, "too_large");
      }
      parts.push(value);
    }
  }
  const buf = new Uint8Array(size);
  let pos = 0;
  for (const p of parts) {
    buf.set(p, pos);
    pos += p.length;
  }
  let v;
  try {
    v = JSON.parse(new TextDecoder().decode(buf));
  } catch (e) {
    throw new Bad("invalid JSON: " + (e instanceof Error ? e.message : String(e)));
  }
  if (v === null) return {};
  if (typeof v !== "object" || Array.isArray(v)) throw new Bad("invalid JSON: not an object");
  return v;
}
__name(readJSONBody, "readJSONBody");
function typeError(k, want) {
  return new Bad(`invalid JSON: ${k} must be ${want}`);
}
__name(typeError, "typeError");
function fStr(o, k) {
  const v = o[k];
  if (v === void 0 || v === null) return "";
  if (typeof v !== "string") throw typeError(k, "a string");
  return v;
}
__name(fStr, "fStr");
function fOptInt(o, k) {
  const v = o[k];
  if (v === void 0 || v === null) return null;
  if (typeof v !== "number" || !Number.isSafeInteger(v)) throw typeError(k, "an integer");
  return v;
}
__name(fOptInt, "fOptInt");
function fInt(o, k) {
  return fOptInt(o, k) ?? 0;
}
__name(fInt, "fInt");
function fBool(o, k) {
  const v = o[k];
  if (v === void 0 || v === null) return false;
  if (typeof v !== "boolean") throw typeError(k, "a boolean");
  return v;
}
__name(fBool, "fBool");
function fBytes(o, k) {
  const v = o[k];
  if (v === void 0 || v === null) return null;
  if (typeof v !== "string") throw typeError(k, "base64");
  const b = decodeStd(v);
  if (!b) throw typeError(k, "base64");
  return b;
}
__name(fBytes, "fBytes");
function fObjects(o, k) {
  const v = o[k];
  if (v === void 0 || v === null) return [];
  if (!Array.isArray(v)) throw typeError(k, "an array");
  return v.map((e) => {
    if (e === null) return {};
    if (typeof e !== "object" || Array.isArray(e)) throw typeError(k, "an array of objects");
    return e;
  });
}
__name(fObjects, "fObjects");

// src/rooms.ts
var GATE_TTL_MS = 6e4;
var gateCache = /* @__PURE__ */ new Map();
function forgetGate(chain) {
  gateCache.delete(chain.toLowerCase());
}
__name(forgetGate, "forgetGate");
function chainStub(env, chain) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}
__name(chainStub, "chainStub");
function roomStub(env, room2) {
  return env.ROOM.get(env.ROOM.idFromName(room2));
}
__name(roomStub, "roomStub");
async function gate(env, chain, device) {
  const key = chain.toLowerCase();
  const now = Date.now();
  const hit = gateCache.get(key)?.get(device);
  if (hit && hit.until > now) return hit.gate;
  const g = await chainStub(env, chain).roomGate(chain, device);
  if (gateCache.size > 1e4) gateCache.clear();
  let byDevice = gateCache.get(key);
  if (!byDevice) gateCache.set(key, byDevice = /* @__PURE__ */ new Map());
  if (byDevice.size > 100) byDevice.clear();
  byDevice.set(device, { gate: g, until: now + GATE_TTL_MS });
  return g;
}
__name(gate, "gate");
var ROOM_ROUTES = [
  [/^$/, ["GET", "DELETE"]],
  [/^\/changes$/, ["GET"]],
  [/^\/records$/, ["POST"]],
  [/^\/invites$/, ["POST"]],
  [/^\/invites\/[^/]+$/, ["DELETE"]],
  [/^\/join$/, ["POST"]],
  [/^\/members$/, ["POST"]],
  [/^\/members\/[^/]+$/, ["PATCH", "DELETE"]],
  [/^\/(owner|successor|epochs|envelopes)$/, ["POST"]]
];
async function rooms(env, request, rest, deps) {
  const method = request.method;
  let kind;
  let roomID = "";
  let sub = "";
  if (rest === "/compass/rooms") {
    if (method !== "GET" && method !== "POST") return text(405, "Method Not Allowed");
    kind = "list";
  } else if (rest.startsWith("/compass/rooms/")) {
    const after = rest.slice("/compass/rooms/".length);
    const slash = after.indexOf("/");
    roomID = slash < 0 ? after : after.slice(0, slash);
    sub = slash < 0 ? "" : after.slice(slash).replace(/\/$/, "");
    if (!roomID) return text(404, "404 page not found");
    const route = ROOM_ROUTES.find(([re]) => re.test(sub));
    if (!route) return text(404, "404 page not found");
    if (!route[1].includes(method)) return text(405, "Method Not Allowed");
    kind = "room";
  } else if (rest === "/compass/identity") {
    if (method !== "GET" && method !== "PUT") return text(405, "Method Not Allowed");
    kind = "identity";
  } else if (rest === "/compass/directory") {
    if (method !== "GET") return text(405, "Method Not Allowed");
    kind = "directory";
  } else {
    return null;
  }
  const chain = await deps.authenticate(env, request.headers.get("Authorization"));
  if (!chain) return fail(401, "invalid sync token");
  const url = new URL(request.url);
  const device = url.searchParams.get("client_id") ?? "";
  const g = await gate(env, chain, device);
  if (g.removed) return deviceRemoved();
  if (g.disabled) return fail(403, "chain_disabled");
  const c = { id: chain.toLowerCase(), listed: await deps.listed(env, chain), device };
  try {
    switch (kind) {
      case "list":
        return method === "GET" ? await listRooms(env, c) : await createRoom(env, request, c);
      case "identity":
        return method === "GET" ? await getIdentity(env, c) : await putIdentity(env, request, c);
      case "directory":
        if (!c.listed) return fail(403, "chain_not_allowed");
        return fail(404, "directory_disabled");
      case "room":
        return await room(env, request, url, c, roomID, sub, deps);
    }
  } catch (e) {
    return errorResponse(e);
  }
}
__name(rooms, "rooms");
async function listRooms(env, c) {
  const out = await chainStub(env, c.id).listRooms(c.listed);
  if (!out) return fail(403, "chain_not_allowed");
  return json(200, out);
}
__name(listRooms, "listRooms");
async function createRoom(env, request, c) {
  if (!c.listed) return fail(403, "chain_not_allowed");
  const body = await readJSONBody(request, MAX_BODY);
  const id = fStr(body, "id");
  const kind = fStr(body, "kind");
  const pub = fBytes(body, "identity_pub");
  const envelope = fBytes(body, "envelope");
  const profile = fBytes(body, "profile");
  if (id !== "" && !validRoomID(id)) throw new Bad("id must be 16 bytes, base64url without padding");
  if (!KINDS.includes(kind)) throw new Bad("kind must be space, folder or easel");
  if (pub?.length !== IDENTITY_KEY_BYTES) throw new Bad("identity_pub must be 32 bytes");
  if (envelope?.length !== ENVELOPE_BYTES) throw new Bad("envelope must be 80 bytes");
  if ((profile?.length ?? 0) > MAX_PROFILE_BYTES) throw new Bad("profile must be at most 4096 bytes");
  if (await chainStub(env, c.id).ownedRoomCount() >= MAX_ROOMS_PER_OWNER) {
    throw new HttpError(409, "too_many_rooms");
  }
  const roomID = id || newID();
  return roomStub(env, roomID).fetch(
    new Request("https://room/create", {
      method: "POST",
      headers: roomHeaders(c, roomID),
      body: JSON.stringify({
        kind,
        identity_pub: b64(pub),
        envelope: b64(envelope),
        profile: profile && profile.length > 0 ? b64(profile) : null
      })
    })
  );
}
__name(createRoom, "createRoom");
async function hasRoomsOrListed(env, c) {
  if (!c.listed && !await chainStub(env, c.id).hasRooms()) throw new HttpError(403, "chain_not_allowed");
}
__name(hasRoomsOrListed, "hasRoomsOrListed");
async function getIdentity(env, c) {
  await hasRoomsOrListed(env, c);
  return json(200, { identity_pub: await chainStub(env, c.id).identityKey() });
}
__name(getIdentity, "getIdentity");
async function putIdentity(env, request, c) {
  await hasRoomsOrListed(env, c);
  const body = await readJSONBody(request, MAX_BODY);
  const pub = fBytes(body, "identity_pub");
  if (pub?.length !== IDENTITY_KEY_BYTES) throw new Bad("identity_pub must be 32 bytes");
  const key = b64(pub);
  const rooms2 = await chainStub(env, c.id).setIdentityKey(key);
  await Promise.all(
    rooms2.map(
      (r) => roomStub(env, r).identityChanged(r, c.id, key, c.device).catch((e) => console.error("rooms: updating an identity key failed", e))
    )
  );
  return noContent();
}
__name(putIdentity, "putIdentity");
function roomHeaders(c, roomID, extra = {}) {
  return new Headers({
    "X-Chain-ID": c.id,
    "X-Listed": c.listed ? "1" : "0",
    "X-Client-ID": c.device,
    "X-Room-ID": roomID,
    ...extra
  });
}
__name(roomHeaders, "roomHeaders");
async function room(env, request, url, c, roomID, sub, deps) {
  if (!validRoomID(roomID)) {
    if (sub === "/join") await readJSONBody(request, MAX_BODY);
    return fail(404, "not_found");
  }
  const extra = {};
  let path = sub || "/";
  const linked = (request.headers.get(LINKED_TOKEN_HEADER) ?? "").trim();
  if (sub === "/successor" || sub === "/owner" && linked !== "") {
    path = "/successor";
    const next = linked ? await deps.authenticate(env, linked.startsWith("Bearer ") ? linked : "Bearer " + linked) : "";
    extra["X-Linked-Chain"] = next.toLowerCase();
  }
  const stub = roomStub(env, roomID);
  const forward = /* @__PURE__ */ __name((headers) => stub.fetch(
    new Request(`https://room${path}${url.search}`, {
      method: request.method,
      headers,
      body: request.method === "GET" || request.method === "DELETE" ? null : request.body
    })
  ), "forward");
  if (sub === "/changes") {
    const w = url.searchParams.get("wait") ?? "";
    const wait = /^\d+$/.test(w) ? Number(w) : 0;
    if (wait > 0 && wait <= MAX_WAIT_SECONDS) return longPoll(request, roomHeaders(c, roomID), wait, forward);
  }
  return forward(roomHeaders(c, roomID, extra));
}
__name(room, "room");
async function longPoll(request, headers, wait, forward) {
  const up = new Headers(headers);
  up.set("Upgrade", "websocket");
  const rsp = await forward(up);
  const ws = rsp.webSocket;
  if (!ws) return rsp;
  ws.accept();
  return new Promise((resolve) => {
    let hello = null;
    let done = false;
    const finish = /* @__PURE__ */ __name((timedOut) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      try {
        ws.close(1e3, "done");
      } catch {
      }
      if (timedOut && hello) {
        resolve(json(200, { seq: hello.seq, epoch: hello.epoch, records: [], more: false }));
      } else {
        resolve(forward(headers));
      }
    }, "finish");
    const timer = setTimeout(() => finish(true), wait * 1e3);
    ws.addEventListener("message", (ev) => {
      if (typeof ev.data !== "string") return;
      if (ev.data === "wake") return finish(false);
      try {
        hello = JSON.parse(ev.data);
      } catch {
        finish(false);
      }
    });
    ws.addEventListener("close", () => finish(false));
    ws.addEventListener("error", () => finish(false));
    request.signal?.addEventListener?.("abort", () => finish(true));
  });
}
__name(longPoll, "longPoll");

// src/access.ts
import { DurableObject } from "cloudflare:workers";
var MAX_LINKS_PER_CHAIN = 64;
var INVITE_TTL_MS = 7 * 864e5;
var MAX_OPEN_INVITES = 50;
var MAX_MEMBERS = 500;
var SCHEMA = `
CREATE TABLE IF NOT EXISTS members (
  chain      TEXT PRIMARY KEY,
  added_at   INTEGER NOT NULL,
  invited_by TEXT NOT NULL,
  name       TEXT NOT NULL DEFAULT ''
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS links (
  chain   TEXT PRIMARY KEY,
  root    TEXT NOT NULL,
  created INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS links_by_root ON links (root);
CREATE TABLE IF NOT EXISTS invites (
  code       TEXT PRIMARY KEY,
  created_by TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  name       TEXT NOT NULL DEFAULT '',
  used_by    TEXT,
  used_at    INTEGER
) WITHOUT ROWID;
`;
function randomCode() {
  const alphabet = "abcdefghijklmnopqrstuvwxyz234567";
  const bytes = crypto.getRandomValues(new Uint8Array(26));
  return Array.from(bytes, (b) => alphabet[b & 31]).join("");
}
__name(randomCode, "randomCode");
var Access = class extends DurableObject {
  static {
    __name(this, "Access");
  }
  sql;
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(SCHEMA);
  }
  owners() {
    return idList(this.env.COMPASS_SYNC_ALLOWED_CHAINS);
  }
  root(chain) {
    const r = this.sql.exec(`SELECT root FROM links WHERE chain = ?`, chain).toArray()[0];
    return r ? r.root : "";
  }
  // Until the owners choose in Compass, COMPASS_SYNC_PUSH_DEFAULT ("off"
  // when the owner said no at setup) decides.
  pushOn() {
    return this.ctx.storage.kv.get("push_relay") ?? this.env.COMPASS_SYNC_PUSH_DEFAULT !== "off";
  }
  listed(chain) {
    const owners = this.owners();
    if (owners.length === 0) return { allowed: true, owner: false };
    if (owners.includes(chain)) return { allowed: true, owner: true };
    const member = this.sql.exec(`SELECT 1 FROM members WHERE chain = ?`, chain).toArray().length > 0;
    return { allowed: member, owner: false };
  }
  // state says whether a chain may sync, directly or through the chain it
  // is linked to, and whether it owns the server.
  state(chainID) {
    return { ...this.listedOrLinked(chainID.toLowerCase()), push: this.pushOn() };
  }
  listedOrLinked(chain) {
    const own = this.listed(chain);
    if (own.allowed) return own;
    const root = this.root(chain);
    if (!root) return { allowed: false, owner: false };
    const blocked = idList(this.env.BLOCKED_CLIENT_IDS);
    if (blocked.includes(root)) return { allowed: false, owner: false };
    return this.listed(root);
  }
  // setSettings changes the owners' settings (owners only): for now
  // {"push_relay": bool}.
  setSettings(byID, settings, relay) {
    if (!this.state(byID).owner) return fail(403, "owner_only");
    if (typeof settings.push_relay !== "boolean") return fail(400, "bad_request");
    this.ctx.storage.kv.put("push_relay", settings.push_relay);
    return json(200, this.settings(relay));
  }
  settings(relay) {
    return { push_relay: this.pushOn(), push_relay_url: relay };
  }
  // link links `other` (a profile account) to `from`'s root (Go:
  // LinkChainToRoot). `from` must be allowed (the Worker checked).
  link(fromID, otherID) {
    const from = fromID.toLowerCase();
    const other = otherID.toLowerCase();
    const root = this.root(from) || from;
    if (other === root || other === from) return new Response(null, { status: 204 });
    if (this.root(other) === root) return new Response(null, { status: 204 });
    const n = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM links WHERE root = ?`, root).one().n);
    if (n >= MAX_LINKS_PER_CHAIN) return fail(429, "too many linked chains");
    this.sql.exec(
      `INSERT INTO links (chain, root, created) VALUES (?, ?, ?)
       ON CONFLICT (chain) DO UPDATE SET root = excluded.root, created = excluded.created`,
      other,
      root,
      Date.now()
    );
    return new Response(null, { status: 204 });
  }
  // createInvite makes a single-use invite code (owners only).
  createInvite(byID, name, origin) {
    const by = byID.toLowerCase();
    if (!this.state(by).owner) return fail(403, "owner_only");
    const now = Date.now();
    this.sql.exec(`DELETE FROM invites WHERE expires_at < ? OR used_by IS NOT NULL`, now);
    const open = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM invites`).one().n);
    if (open >= MAX_OPEN_INVITES) return fail(429, "too many open invites");
    const code = randomCode();
    this.sql.exec(
      `INSERT INTO invites (code, created_by, created_at, expires_at, name) VALUES (?, ?, ?, ?, ?)`,
      code,
      by,
      now,
      now + INVITE_TTL_MS,
      name.slice(0, 100)
    );
    return json(201, { code, link: `${origin}/invite#${code}`, expires_in: Math.floor(INVITE_TTL_MS / 1e3) });
  }
  // join adds the caller with an invite code.
  join(chainID, code) {
    const chain = chainID.toLowerCase();
    if (this.state(chain).allowed) return new Response(null, { status: 204 });
    const now = Date.now();
    const inv = this.sql.exec(`SELECT * FROM invites WHERE code = ?`, code).toArray()[0];
    if (!inv) return fail(404, "no_such_invite");
    if (inv.used_by !== null || Number(inv.expires_at) < now) return fail(410, "invite_used_or_expired");
    if (Number(this.sql.exec(`SELECT COUNT(*) AS n FROM members`).one().n) >= MAX_MEMBERS) {
      return fail(429, "too many members");
    }
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`UPDATE invites SET used_by = ?, used_at = ? WHERE code = ?`, chain, now, code);
      this.sql.exec(
        `INSERT INTO members (chain, added_at, invited_by, name) VALUES (?, ?, ?, ?)
         ON CONFLICT (chain) DO NOTHING`,
        chain,
        now,
        inv.created_by,
        inv.name
      );
    });
    return new Response(null, { status: 204 });
  }
  // members lists the friends and open invites (owners only).
  members(byID, relay) {
    if (!this.state(byID).owner) return fail(403, "owner_only");
    const now = Date.now();
    const members = this.sql.exec(`SELECT chain, added_at, name FROM members ORDER BY added_at`).toArray().map((r) => ({
      chain: r.chain,
      name: r.name,
      added_at: Math.floor(Number(r.added_at) / 1e3),
      linked: Number(this.sql.exec(`SELECT COUNT(*) AS n FROM links WHERE root = ?`, r.chain).one().n)
    }));
    const invites = this.sql.exec(`SELECT code, name, expires_at FROM invites WHERE used_by IS NULL AND expires_at >= ? ORDER BY created_at`, now).toArray().map((r) => ({ code: r.code, name: r.name, expires_in: Math.floor((Number(r.expires_at) - now) / 1e3) }));
    return json(200, { owners: this.owners(), members, invites, settings: this.settings(relay) });
  }
  // remove takes a friend off the list (their linked profiles go with
  // them), or revokes an open invite.
  remove(byID, target) {
    if (!this.state(byID).owner) return fail(403, "owner_only");
    const t = target.toLowerCase();
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`DELETE FROM members WHERE chain = ?`, t);
      this.sql.exec(`DELETE FROM links WHERE root = ?`, t);
      this.sql.exec(`DELETE FROM invites WHERE code = ? AND used_by IS NULL`, target);
    });
    return new Response(null, { status: 204 });
  }
};

// src/chain.ts
import { DurableObject as DurableObject2 } from "cloudflare:workers";

// src/push.ts
var SEND_TAB_TYPE = 601980;
var BACKGROUND_TYPES = /* @__PURE__ */ new Set([
  97e4,
  // COMPASS_SPACES
  45873,
  // PASSWORDS
  48119
  // EXTENSIONS
]);
var MAX_REGISTRATIONS = 20;
function parseRegistration(raw, cfg) {
  let body;
  try {
    body = JSON.parse(raw);
    if (!body || typeof body !== "object") throw new Error();
  } catch {
    return "JSON body required";
  }
  const platform = body.platform ?? "apns";
  const topic = typeof body.topic === "string" ? body.topic : "";
  let token = typeof body.token === "string" ? body.token : "";
  if (platform === "fcm") {
    if (!(token.length >= 32 && token.length <= 4096 && /^[A-Za-z0-9:_-]+$/.test(token))) {
      return "token: FCM registration token required";
    }
    if (!cfg.pushTopics.includes(topic)) return "topic: not an app this server pushes to";
    return { token, environment: "", topic, platform: "fcm" };
  }
  if (platform !== "apns") return `platform: "apns" or "fcm"`;
  token = token.toLowerCase();
  if (!(token.length >= 32 && token.length <= 200 && token.length % 2 === 0 && /^[0-9a-f]+$/.test(token))) {
    return "token: hex device token required";
  }
  if (body.environment !== "sandbox" && body.environment !== "production") {
    return `environment: "sandbox" or "production"`;
  }
  if (!cfg.pushTopics.includes(topic)) return "topic: not an app this server pushes to";
  return { token, environment: body.environment, topic, platform: "apns" };
}
__name(parseRegistration, "parseRegistration");
async function tokenHash(platform, token) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${platform}:${token}`));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
__name(tokenHash, "tokenHash");
var TIMEOUT_MS = 15e3;
async function seal(relay, server, reg) {
  try {
    const res = await fetch(`${relay}/v1/seal`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ server, ...reg }),
      signal: AbortSignal.timeout(TIMEOUT_MS)
    });
    if (res.status === 400 || res.status === 403) {
      await res.body?.cancel();
      return "refused";
    }
    if (res.status !== 200) {
      await res.body?.cancel();
      return null;
    }
    const { handle } = await res.json();
    return typeof handle === "string" && handle ? handle : null;
  } catch {
    return null;
  }
}
__name(seal, "seal");
async function relayPush(relay, server, handle, type) {
  try {
    const res = await fetch(`${relay}/v1/push`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ server, handle, type }),
      signal: AbortSignal.timeout(TIMEOUT_MS)
    });
    let error = "";
    try {
      error = (await res.json()).error ?? "";
    } catch {
    }
    if (res.status === 200) return "sent";
    if (res.status === 410 || error === "bad_handle") return "drop";
    console.warn(`push relay: ${type} refused, ${res.status} ${error}`);
    return "failed";
  } catch (e) {
    console.warn(`push relay: ${type} failed, ${String(e)}`);
    return "failed";
  }
}
__name(relayPush, "relayPush");

// src/proto.ts
var WIRE_VARINT = 0;
var WIRE_I64 = 1;
var WIRE_LEN = 2;
var WIRE_START_GROUP = 3;
var WIRE_END_GROUP = 4;
var WIRE_I32 = 5;
var ProtoError = class extends Error {
  static {
    __name(this, "ProtoError");
  }
};
var EMPTY = new Uint8Array(0);
function readVarint(buf, pos) {
  let value = 0;
  let mul = 1;
  for (let i = 0; i < 10; i++) {
    if (pos >= buf.length) throw new ProtoError("truncated varint");
    const b = buf[pos++];
    value += (b & 127) * mul;
    if (b < 128) return [value, pos];
    mul *= 128;
  }
  throw new ProtoError("varint too long");
}
__name(readVarint, "readVarint");
function* fields(buf) {
  let pos = 0;
  while (pos < buf.length) {
    let tag;
    [tag, pos] = readVarint(buf, pos);
    const num2 = Math.floor(tag / 8);
    const wire = tag & 7;
    if (num2 < 1) throw new ProtoError("bad field number");
    switch (wire) {
      case WIRE_VARINT: {
        let v;
        [v, pos] = readVarint(buf, pos);
        yield { num: num2, wire, int: v, bytes: EMPTY };
        break;
      }
      case WIRE_I64:
        if (pos + 8 > buf.length) throw new ProtoError("truncated fixed64");
        yield { num: num2, wire, int: 0, bytes: buf.subarray(pos, pos + 8) };
        pos += 8;
        break;
      case WIRE_I32:
        if (pos + 4 > buf.length) throw new ProtoError("truncated fixed32");
        yield { num: num2, wire, int: 0, bytes: buf.subarray(pos, pos + 4) };
        pos += 4;
        break;
      case WIRE_LEN: {
        let len2;
        [len2, pos] = readVarint(buf, pos);
        if (pos + len2 > buf.length) throw new ProtoError("truncated bytes");
        yield { num: num2, wire, int: 0, bytes: buf.subarray(pos, pos + len2) };
        pos += len2;
        break;
      }
      case WIRE_START_GROUP: {
        const start = pos;
        const end = skipGroup(buf, pos, num2);
        yield { num: num2, wire, int: 0, bytes: buf.subarray(start, end.contentEnd) };
        pos = end.next;
        break;
      }
      default:
        throw new ProtoError(`unexpected wire type ${wire}`);
    }
  }
}
__name(fields, "fields");
function skipGroup(buf, pos, num2) {
  for (; ; ) {
    const before = pos;
    let tag;
    [tag, pos] = readVarint(buf, pos);
    const wire = tag & 7;
    const n = Math.floor(tag / 8);
    switch (wire) {
      case WIRE_END_GROUP:
        if (n !== num2) throw new ProtoError("mismatched group end");
        return { contentEnd: before, next: pos };
      case WIRE_VARINT:
        [, pos] = readVarint(buf, pos);
        break;
      case WIRE_I64:
        pos += 8;
        break;
      case WIRE_I32:
        pos += 4;
        break;
      case WIRE_LEN: {
        let len2;
        [len2, pos] = readVarint(buf, pos);
        pos += len2;
        break;
      }
      case WIRE_START_GROUP:
        pos = skipGroup(buf, pos, n).next;
        break;
      default:
        throw new ProtoError(`unexpected wire type ${wire}`);
    }
    if (pos > buf.length) throw new ProtoError("truncated group");
  }
}
__name(skipGroup, "skipGroup");
var decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
var encoder = new TextEncoder();
function str(f) {
  if (f.wire !== WIRE_LEN) throw new ProtoError(`field ${f.num} is not a string`);
  try {
    return decoder.decode(f.bytes);
  } catch {
    throw new ProtoError(`field ${f.num} is not UTF-8`);
  }
}
__name(str, "str");
function int(f) {
  if (f.wire !== WIRE_VARINT) throw new ProtoError(`field ${f.num} is not a varint`);
  return f.int >= 2 ** 63 ? f.int - 2 ** 64 : f.int;
}
__name(int, "int");
function bool(f) {
  return int(f) !== 0;
}
__name(bool, "bool");
function len(f) {
  if (f.wire !== WIRE_LEN) throw new ProtoError(`field ${f.num} is not length-delimited`);
  return f.bytes;
}
__name(len, "len");
var Writer = class {
  static {
    __name(this, "Writer");
  }
  // Small values collect in `small`; byte arrays are kept as they are, so a
  // response of large specifics isn't copied byte by byte.
  parts = [];
  small = [];
  size = 0;
  flush() {
    if (this.small.length) {
      this.parts.push(Uint8Array.from(this.small));
      this.small = [];
    }
  }
  raw(v) {
    this.flush();
    this.parts.push(v);
    this.size += v.length;
  }
  varintRaw(v) {
    if (v < 0) {
      let big = BigInt.asUintN(64, BigInt(Math.trunc(v)));
      while (big >= 0x80n) {
        this.small.push(Number(big & 0x7fn) | 128);
        this.size++;
        big >>= 7n;
      }
      this.small.push(Number(big));
      this.size++;
      return;
    }
    while (v >= 128) {
      this.small.push(v % 128 | 128);
      this.size++;
      v = Math.floor(v / 128);
    }
    this.small.push(v);
    this.size++;
  }
  tag(num2, wire) {
    this.varintRaw(num2 * 8 + wire);
  }
  int(num2, v) {
    if (v === void 0 || v === null) return this;
    this.tag(num2, WIRE_VARINT);
    this.varintRaw(v);
    return this;
  }
  bool(num2, v) {
    if (v === void 0 || v === null) return this;
    return this.int(num2, v ? 1 : 0);
  }
  bytes(num2, v) {
    if (v === void 0 || v === null) return this;
    this.tag(num2, WIRE_LEN);
    this.varintRaw(v.length);
    this.raw(v);
    return this;
  }
  string(num2, v) {
    if (v === void 0 || v === null) return this;
    return this.bytes(num2, encoder.encode(v));
  }
  message(num2, w) {
    return this.bytes(num2, w.finish());
  }
  group(num2, w) {
    this.tag(num2, WIRE_START_GROUP);
    this.raw(w.finish());
    this.tag(num2, WIRE_END_GROUP);
    return this;
  }
  finish() {
    this.flush();
    if (this.parts.length === 1) return this.parts[0];
    const out = new Uint8Array(this.size);
    let pos = 0;
    for (const p of this.parts) {
      out.set(p, pos);
      pos += p.length;
    }
    return out;
  }
};
var Contents = { COMMIT: 1, GET_UPDATES: 2, CLEAR_SERVER_DATA: 5 };
var ErrorType = {
  SUCCESS: 0,
  THROTTLED: 3,
  TRANSIENT_ERROR: 8,
  DISABLED_BY_ADMIN: 10
};
var Origin = { PERIODIC: 4, NEW_CLIENT: 9 };
var ResponseType = {
  SUCCESS: 1,
  CONFLICT: 2,
  INVALID_MESSAGE: 4,
  OVER_QUOTA: 5,
  TRANSIENT_ERROR: 6
};
function decodeSyncEntity(buf) {
  const e = {};
  for (const f of fields(buf)) {
    switch (f.num) {
      case 1:
        e.idString = str(f);
        break;
      case 2:
        e.parentIdString = str(f);
        break;
      case 4:
        e.version = int(f);
        break;
      case 5:
        e.mtime = int(f);
        break;
      case 6:
        e.ctime = int(f);
        break;
      case 7:
        e.name = str(f);
        break;
      case 8:
        e.nonUniqueName = str(f);
        break;
      case 10:
        e.serverDefinedUniqueTag = str(f);
        break;
      case 18:
        e.deleted = bool(f);
        break;
      case 19:
        e.originatorCacheGuid = str(f);
        break;
      case 20:
        e.originatorClientItemId = str(f);
        break;
      case 21:
        e.specifics = len(f);
        break;
      case 22:
        e.folder = bool(f);
        break;
      case 23:
        e.clientTagHash = str(f);
        break;
      case 25:
        e.uniquePosition = len(f);
        break;
    }
  }
  return e;
}
__name(decodeSyncEntity, "decodeSyncEntity");
function encodeSyncEntity(e) {
  return new Writer().string(1, e.idString).string(2, e.parentIdString).int(4, e.version).int(5, e.mtime).int(6, e.ctime).string(7, e.name).string(8, e.nonUniqueName).string(10, e.serverDefinedUniqueTag).bool(18, e.deleted).string(19, e.originatorCacheGuid).string(20, e.originatorClientItemId).bytes(21, e.specifics).bool(22, e.folder).string(23, e.clientTagHash).bytes(25, e.uniquePosition);
}
__name(encodeSyncEntity, "encodeSyncEntity");
function decodeProgressMarker(buf) {
  const m = {};
  for (const f of fields(buf)) {
    if (f.num === 1) m.dataTypeId = int(f);
    else if (f.num === 2) m.token = len(f);
  }
  return m;
}
__name(decodeProgressMarker, "decodeProgressMarker");
function decodeGetUpdates(buf) {
  const m = { fromProgressMarker: [] };
  for (const f of fields(buf)) {
    if (f.num === 3) m.fetchFolders = bool(f);
    else if (f.num === 6) m.fromProgressMarker.push(decodeProgressMarker(len(f)));
    else if (f.num === 9) m.origin = int(f);
  }
  return m;
}
__name(decodeGetUpdates, "decodeGetUpdates");
function decodeCommit(buf) {
  const m = { entries: [] };
  for (const f of fields(buf)) {
    if (f.num === 1) m.entries.push(decodeSyncEntity(len(f)));
    else if (f.num === 2) m.cacheGuid = str(f);
  }
  return m;
}
__name(decodeCommit, "decodeCommit");
function decodeClientToServerMessage(buf) {
  const m = {};
  for (const f of fields(buf)) {
    if (f.num === 3) m.messageContents = int(f);
    else if (f.num === 4) m.commit = decodeCommit(len(f));
    else if (f.num === 5) m.getUpdates = decodeGetUpdates(len(f));
  }
  return m;
}
__name(decodeClientToServerMessage, "decodeClientToServerMessage");
function encodeClientToServerResponse(r) {
  const w = new Writer();
  if (r.commit) {
    const c = new Writer();
    for (const e of r.commit.entryResponse) {
      c.group(1, new Writer().int(2, e.responseType).string(3, e.idString).int(6, e.version).string(9, e.errorMessage).int(10, e.mtime));
    }
    w.message(1, c);
  }
  if (r.getUpdates) {
    const g = new Writer();
    for (const e of r.getUpdates.entries) g.message(1, encodeSyncEntity(e));
    g.int(4, r.getUpdates.changesRemaining);
    for (const m of r.getUpdates.newProgressMarker) {
      g.message(5, new Writer().int(1, m.dataTypeId).bytes(2, m.token));
    }
    for (const k of r.getUpdates.encryptionKeys) g.bytes(6, k);
    w.message(2, g);
  }
  w.int(4, r.errorCode);
  w.string(5, r.errorMessage);
  w.string(6, r.storeBirthday);
  if (r.clientCommand) {
    w.message(7, new Writer().int(1, r.clientCommand.setSyncPollInterval).int(3, r.clientCommand.maxCommitBatchSize));
  }
  if (r.clearServerData) w.message(15, new Writer());
  return w.finish();
}
__name(encodeClientToServerResponse, "encodeClientToServerResponse");
function dataTypeOfSpecifics(specifics) {
  for (const f of fields(specifics)) {
    if (f.wire === WIRE_LEN && f.num !== 1) return f.num;
  }
  throw new ProtoError("specifics have no data type field");
}
__name(dataTypeOfSpecifics, "dataTypeOfSpecifics");
function emptySpecifics(dataType) {
  return new Writer().bytes(dataType, EMPTY).finish();
}
__name(emptySpecifics, "emptySpecifics");
function encodeToken(mtime) {
  const out = new Uint8Array(10);
  let u = BigInt(mtime) >= 0n ? BigInt(mtime) * 2n : -BigInt(mtime) * 2n - 1n;
  let i = 0;
  while (u >= 0x80n) {
    out[i++] = Number(u & 0x7fn) | 128;
    u >>= 7n;
  }
  out[i] = Number(u);
  return out;
}
__name(encodeToken, "encodeToken");
function decodeToken(token) {
  let u = 0n;
  let shift = 0n;
  for (let i = 0; i < token.length && i < 10; i++) {
    const b = BigInt(token[i]);
    u |= (b & 0x7fn) << shift;
    if (b < 0x80n) {
      const v = u & 1n ? -(u >> 1n) - 1n : u >> 1n;
      return Number(v);
    }
    shift += 7n;
  }
  throw new ProtoError("bad progress token");
}
__name(decodeToken, "decodeToken");

// src/chain.ts
var BOOKMARK_TYPE = 32904;
var NIGORI_TYPE = 47745;
var DEVICE_INFO_TYPE = 154522;
var HISTORY_TYPE = 963985;
var HISTORY_DELETE_DIRECTIVE_TYPE = 150251;
var STORE_BIRTHDAY = "1";
var MAX_COMMIT_BATCH_SIZE = 90;
var MAX_GU_BATCH_SIZE = 500;
var TIE_FALLBACK_SIZE = 100;
var PAYLOAD_LIMIT = 10 * 1024 * 1024;
var MAX_ACTIVE_DEVICES = 50;
var HIGH_MAX_ACTIVE_DEVICES = 100;
var MAX_STREAMS = 20;
var MAX_CLIENT_ID = 128;
var BYTES_FULL_MARGIN = 64 << 10;
var TOUCH_INTERVAL_MS = 36e5;
var PURGE_INTERVAL_MS = 36e5;
var QUOTA_ENTITIES = 1;
var QUOTA_BYTES = 2;
var QUOTA_HISTORY = 4;
var SCHEMA2 = `
CREATE TABLE IF NOT EXISTS entities (
  id                        TEXT PRIMARY KEY,
  parent_id                 TEXT,
  version                   INTEGER,
  mtime                     INTEGER,
  ctime                     INTEGER,
  name                      TEXT,
  non_unique_name           TEXT,
  server_defined_unique_tag TEXT,
  deleted                   INTEGER,
  originator_cache_guid     TEXT,
  originator_client_item_id TEXT,
  specifics                 BLOB,
  data_type                 INTEGER,
  folder                    INTEGER,
  client_defined_unique_tag TEXT,
  unique_position           BLOB,
  expiration_time           INTEGER,
  size                      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS entities_by_type_mtime ON entities (data_type, mtime);
CREATE INDEX IF NOT EXISTS entities_by_client_tag ON entities (client_defined_unique_tag)
  WHERE client_defined_unique_tag IS NOT NULL;
CREATE INDEX IF NOT EXISTS entities_by_expiration ON entities (expiration_time)
  WHERE expiration_time > 0;
CREATE INDEX IF NOT EXISTS tombstones_by_mtime ON entities (mtime) WHERE deleted = 1;
CREATE TABLE IF NOT EXISTS unique_tags (
  id    TEXT PRIMARY KEY,
  mtime INTEGER,
  ctime INTEGER
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS chain (
  one                     INTEGER PRIMARY KEY CHECK (one = 1),
  chain_id                TEXT,
  bytes                   INTEGER NOT NULL DEFAULT 0,
  disabled_at             INTEGER,
  last_active             INTEGER,
  quota_full              INTEGER NOT NULL DEFAULT 0,
  quota_full_time         INTEGER,
  item_count              INTEGER NOT NULL DEFAULT 0,
  history_period1         INTEGER NOT NULL DEFAULT 0,
  history_period2         INTEGER NOT NULL DEFAULT 0,
  history_period3         INTEGER NOT NULL DEFAULT 0,
  history_period4         INTEGER NOT NULL DEFAULT 0,
  last_period_change_time INTEGER NOT NULL DEFAULT 0
);
INSERT OR IGNORE INTO chain (one) VALUES (1);
CREATE TABLE IF NOT EXISTS removed_devices (
  cache_guid TEXT PRIMARY KEY,
  removed_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS room_index (
  room_id TEXT PRIMARY KEY,
  kind    TEXT NOT NULL,
  owner   TEXT NOT NULL,
  role    TEXT NOT NULL,
  pending INTEGER NOT NULL,
  epoch   INTEGER NOT NULL,
  seq     INTEGER NOT NULL
) WITHOUT ROWID;
-- Devices' push registrations, as relay handles (push.ts): never the
-- token itself, only its hash, to find a device that registered again
-- under a new cache GUID. bg_last and bg_due space background pushes.
CREATE TABLE IF NOT EXISTS push_handles (
  cache_guid TEXT PRIMARY KEY,
  handle     TEXT NOT NULL,
  token_hash TEXT NOT NULL,
  platform   TEXT NOT NULL,
  server     TEXT NOT NULL,
  relay      TEXT NOT NULL,
  updated    INTEGER NOT NULL,
  bg_last    INTEGER NOT NULL DEFAULT 0,
  bg_due     INTEGER
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS identity (
  one     INTEGER PRIMARY KEY CHECK (one = 1),
  pub     TEXT NOT NULL,
  updated INTEGER NOT NULL
);
`;
var ENTITY_COLUMNS = `id, parent_id, version, mtime, ctime, name, non_unique_name,
  server_defined_unique_tag, deleted, originator_cache_guid, originator_client_item_id,
  specifics, data_type, folder, client_defined_unique_tag, unique_position, expiration_time`;
var QuotaExceeded = class extends Error {
  static {
    __name(this, "QuotaExceeded");
  }
};
var InsertError = class extends Error {
  constructor(message, conflict) {
    super(message);
    this.conflict = conflict;
  }
  conflict;
  static {
    __name(this, "InsertError");
  }
};
var encoder2 = new TextEncoder();
function utf8Len(s) {
  return s ? encoder2.encode(s).length : 0;
}
__name(utf8Len, "utf8Len");
function entitySize(specifics, pos, name, nun) {
  return (specifics?.length ?? 0) + (pos?.length ?? 0) + utf8Len(name) + utf8Len(nun);
}
__name(entitySize, "entitySize");
function blob(b) {
  if (!b || b.length === 0) return null;
  return b.slice().buffer;
}
__name(blob, "blob");
function boolCol(b) {
  return b === void 0 ? null : b ? 1 : 0;
}
__name(boolCol, "boolCol");
function opt(v) {
  return v === null ? void 0 : v;
}
__name(opt, "opt");
function bytesOf(v) {
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (v instanceof Uint8Array) return v;
  return void 0;
}
__name(bytesOf, "bytesOf");
function rowToEntity(r) {
  const flag = /* @__PURE__ */ __name((v) => v === null ? void 0 : Number(v) !== 0, "flag");
  return {
    idString: r.id,
    parentIdString: opt(r.parent_id),
    version: opt(r.version),
    mtime: opt(r.mtime),
    ctime: opt(r.ctime),
    name: opt(r.name),
    nonUniqueName: opt(r.non_unique_name),
    serverDefinedUniqueTag: opt(r.server_defined_unique_tag),
    clientTagHash: opt(r.client_defined_unique_tag),
    originatorCacheGuid: opt(r.originator_cache_guid),
    originatorClientItemId: opt(r.originator_client_item_id),
    deleted: flag(r.deleted),
    folder: flag(r.folder),
    specifics: bytesOf(r.specifics) ?? new Uint8Array(0),
    uniquePosition: bytesOf(r.unique_position)
  };
}
__name(rowToEntity, "rowToEntity");
async function clientTagHash(dataType, tag) {
  const prefix = emptySpecifics(dataType);
  const t = encoder2.encode(tag);
  const input = new Uint8Array(prefix.length + t.length);
  input.set(prefix);
  input.set(t, prefix.length);
  const sum = new Uint8Array(await crypto.subtle.digest("SHA-1", input));
  return btoa(String.fromCharCode(...sum));
}
__name(clientTagHash, "clientTagHash");
var Chain = class extends DurableObject2 {
  static {
    __name(this, "Chain");
  }
  sql;
  cfg;
  lastMtime = 0;
  lastTouch = 0;
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.cfg = loadConfig(env);
    this.sql.exec(SCHEMA2);
    const row = this.sql.exec(`SELECT MAX(mtime) AS m FROM entities`).one();
    this.lastMtime = Number(row.m ?? 0);
  }
  // nextMtime: strictly increasing mtimes in ms (datastore/mtime.go), so a
  // page of updates never ends inside a run of equal mtimes.
  nextMtime() {
    let now = Date.now();
    if (now <= this.lastMtime) now = this.lastMtime + 1;
    this.lastMtime = now;
    return now;
  }
  get historySecs() {
    return this.cfg.historyDays * 86400;
  }
  // --- HTTP ------------------------------------------------------------------
  async fetch(request) {
    const url = new URL(request.url);
    const chainID = request.headers.get("X-Chain-ID") ?? "";
    if (!chainID) return text(401, "Unauthorized");
    this.rememberChainID(chainID);
    const client = url.searchParams.get("client_id") ?? "";
    if (client && this.isDeviceRemoved(client)) return deviceRemoved();
    this.followPushChoice(request.headers.get("X-Compass-Push"));
    const path = url.pathname;
    if (path === "/command/" && request.method === "POST") return this.command(request, chainID);
    if (path === "/events" && request.method === "GET") return this.events(request, client);
    if (path === "/quota" && request.method === "GET") return this.quota();
    if (path.startsWith("/devices") && request.method === "DELETE") return this.removeDevice(url, client);
    if (path === "/push") return this.register(request, client);
    return text(404, "404 page not found");
  }
  rememberChainID(chainID) {
    const row = this.sql.exec(`SELECT chain_id FROM chain`).one();
    if (row.chain_id !== chainID) this.sql.exec(`UPDATE chain SET chain_id = ?`, chainID);
  }
  isDisabled() {
    return this.sql.exec(`SELECT disabled_at FROM chain`).one().disabled_at !== null;
  }
  // touch records that a device talked to the server (chains.last_active,
  // at most hourly) and keeps the maintenance alarm set.
  async touch() {
    const now = Date.now();
    if (now - this.lastTouch < TOUCH_INTERVAL_MS) return;
    this.lastTouch = now;
    this.sql.exec(`UPDATE chain SET last_active = ?`, Math.floor(now / 1e3));
    if (await this.ctx.storage.getAlarm() === null) {
      this.ctx.storage.kv.put("purge_at", now + PURGE_INTERVAL_MS);
      await this.ctx.storage.setAlarm(now + PURGE_INTERVAL_MS);
    }
  }
  async command(request, chainID) {
    if (this.isDisabled()) {
      return protoResponse({ errorCode: ErrorType.DISABLED_BY_ADMIN });
    }
    await this.touch();
    let body;
    try {
      body = await readBody(request);
    } catch {
      return text(500, "Read request body error");
    }
    let msg;
    try {
      msg = decodeClientToServerMessage(body);
    } catch {
      return text(500, "Unmarshal error");
    }
    const rsp = {
      storeBirthday: STORE_BIRTHDAY,
      clientCommand: { setSyncPollInterval: this.cfg.pollSeconds, maxCommitBatchSize: MAX_COMMIT_BATCH_SIZE }
    };
    try {
      switch (msg.messageContents) {
        case Contents.GET_UPDATES:
          this.handleGetUpdates(msg, rsp, chainID);
          break;
        case Contents.COMMIT: {
          if (!msg.commit) throw new Error("nil commitMsg is received");
          const isNew = msg.commit.entries.map((e) => (e.version ?? 0) === 0 && !e.deleted);
          this.handleCommit(msg.commit.entries, msg.commit.cacheGuid, rsp);
          this.notifyCommit(msg, rsp, isNew);
          break;
        }
        case Contents.CLEAR_SERVER_DATA:
          await this.disable();
          rsp.clearServerData = true;
          rsp.errorCode = ErrorType.SUCCESS;
          break;
        default:
          return text(500, "unsupported message type of ClientToServerMessage");
      }
    } catch (e) {
      if (e instanceof CodedError) {
        rsp.errorCode = e.code;
        rsp.errorMessage = e.message;
      } else {
        console.error("command failed", e);
        return text(500, String(e instanceof Error ? e.message : e));
      }
    }
    return protoResponse(rsp);
  }
  // --- GetUpdates ------------------------------------------------------------
  handleGetUpdates(msg, rsp, chainID) {
    const gu = msg.getUpdates ?? { fromProgressMarker: [] };
    const isNewClient = gu.origin === Origin.NEW_CLIENT;
    const g = {
      entries: [],
      changesRemaining: 0,
      newProgressMarker: [],
      encryptionKeys: []
    };
    rsp.getUpdates = g;
    rsp.errorCode = ErrorType.SUCCESS;
    if (isNewClient) this.setupNewClient(chainID);
    if (gu.fromProgressMarker.length === 0) return;
    const fetchFolders = gu.fetchFolders ?? true;
    for (const marker of gu.fromProgressMarker) {
      const dataType = marker.dataTypeId ?? 0;
      const token = marker.token && marker.token.length > 0 ? marker.token : encodeToken(0);
      const out = { dataTypeId: dataType, token };
      g.newProgressMarker.push(out);
      if (dataType === NIGORI_TYPE && isNewClient) {
        g.encryptionKeys = [encoder2.encode("1234")];
      }
      if (g.entries.length >= MAX_GU_BATCH_SIZE) continue;
      let since;
      try {
        since = decodeToken(token);
      } catch {
        throw new Error(`failed at decoding token value`);
      }
      const max = MAX_GU_BATCH_SIZE - g.entries.length;
      const { more, entities } = this.getUpdatesForType(dataType, since, fetchFolders, max);
      if (isNewClient && dataType === NIGORI_TYPE && since === 0 && entities.length === 0) {
        throw new CodedError(ErrorType.TRANSIENT_ERROR, "nigori root folder entity is not ready yet");
      }
      if (more) g.changesRemaining = 1;
      if (entities.length > 0) {
        g.entries.push(...entities);
        out.token = encodeToken(entities[entities.length - 1].mtime);
      }
    }
  }
  // getUpdatesForType returns up to max entities of a type changed after
  // `since`, oldest first, without expired ones; a page never ends inside a
  // run of equal mtimes (datastore.SQLite.GetUpdatesForType).
  getUpdatesForType(dataType, since, fetchFolders, max) {
    if (max <= 0) return { more: true, entities: [] };
    let q = `SELECT ${ENTITY_COLUMNS} FROM entities
      WHERE data_type = ? AND mtime > ?
        AND (expiration_time IS NULL OR expiration_time <= 0 OR expiration_time >= ?)`;
    if (!fetchFolders) q += ` AND folder = 0`;
    q += ` ORDER BY mtime, rowid LIMIT ?`;
    const rows = this.sql.exec(q, dataType, since, Math.floor(Date.now() / 1e3), max + 1).toArray();
    let entities = rows.map(rowToEntity);
    const more = entities.length >= max;
    if (entities.length > max) {
      const next = entities[max].mtime;
      entities = entities.slice(0, max);
      let k = entities.length;
      while (k > 0 && entities[k - 1].mtime === next) k--;
      if (k > 0) entities = entities.slice(0, k);
      else if (max < TIE_FALLBACK_SIZE) entities = [];
    }
    return { more, entities };
  }
  // setupNewClient refuses a new device past the device limit (THROTTLED)
  // and creates the server-defined folders on a new chain.
  setupNewClient(chainID) {
    const active = Number(
      this.sql.exec(
        `SELECT COUNT(*) AS n FROM entities
         WHERE data_type = ? AND folder = 0 AND COALESCE(deleted, 0) = 0
           AND (expiration_time IS NULL OR expiration_time <= 0 OR expiration_time >= ?)`,
        DEVICE_INFO_TYPE,
        Math.floor(Date.now() / 1e3)
      ).one().n
    );
    const limit = this.cfg.highDeviceLimit.has(chainID.toLowerCase()) ? HIGH_MAX_ACTIVE_DEVICES : MAX_ACTIVE_DEVICES;
    if (active >= limit) {
      throw new CodedError(ErrorType.THROTTLED, "exceed limit of active devices in a chain");
    }
    this.insertServerDefinedEntities();
  }
  insertServerDefinedEntities() {
    const has = this.sql.exec(`SELECT 1 FROM unique_tags WHERE id = ?`, "Server#google_chrome_nigori").toArray();
    if (has.length > 0) return;
    const now = Date.now();
    const make = /* @__PURE__ */ __name((name, tag, parentId, dataType) => ({
      id: crypto.randomUUID(),
      parentId,
      version: 1,
      mtime: this.nextMtime(),
      ctime: now,
      name,
      serverDefinedUniqueTag: tag,
      deleted: false,
      folder: true,
      specifics: emptySpecifics(dataType),
      dataType
    }), "make");
    const root = make("Bookmarks", "google_chrome_bookmarks", "0", BOOKMARK_TYPE);
    const entities = [
      make("Nigori", "google_chrome_nigori", "0", NIGORI_TYPE),
      root,
      make("Other Bookmarks", "other_bookmarks", root.id, BOOKMARK_TYPE),
      make("Synced Bookmarks", "synced_bookmarks", root.id, BOOKMARK_TYPE),
      make("Bookmark Bar", "bookmark_bar", root.id, BOOKMARK_TYPE)
    ];
    this.ctx.storage.transactionSync(() => {
      for (const e of entities) {
        if (!this.insertTag("Server#" + e.serverDefinedUniqueTag)) {
          throw new CodedError(ErrorType.TRANSIENT_ERROR, `tag ${e.serverDefinedUniqueTag} in use`);
        }
        this.writeNewEntity(e);
        const size = entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName);
        this.sql.exec(`UPDATE chain SET bytes = bytes + ?`, size);
      }
    });
  }
  // --- Commit ----------------------------------------------------------------
  handleCommit(entries, cacheGuid, rsp) {
    const responses = [];
    rsp.commit = { entryResponse: responses };
    rsp.errorCode = ErrorType.SUCCESS;
    if (entries.length === 0) return;
    const counts = this.readCounts();
    const state = {
      currentNormal: counts.itemCount,
      currentHistory: counts.h1 + counts.h2 + counts.h3 + counts.h4,
      newNormal: 0,
      newHistory: 0,
      boosted: 0,
      refused: 0,
      passed: 0
    };
    if (state.currentHistory > this.cfg.maxHistory) {
      state.boosted = Math.min(this.cfg.maxEntities - this.cfg.maxHistory, state.currentHistory - this.cfg.maxHistory);
    }
    const idMap = /* @__PURE__ */ new Map();
    for (const pb of entries) {
      const r = {};
      responses.push(r);
      this.commitEntry(pb, cacheGuid, r, idMap, state);
    }
    this.recordQuota(state.refused, state.passed);
    counts.itemCount += state.newNormal;
    counts.h4 += state.newHistory;
    this.writeCounts(counts);
  }
  commitEntry(pb, cacheGuid, r, idMap, state) {
    let entity;
    try {
      entity = this.toDBEntity(pb, cacheGuid);
    } catch (e) {
      r.responseType = ResponseType.INVALID_MESSAGE;
      r.errorMessage = `Cannot convert protobuf sync entity to DB format: ${e instanceof Error ? e.message : e}`;
      return;
    }
    if (entity.parentId !== void 0 && idMap.has(entity.parentId)) {
      entity.parentId = idMap.get(entity.parentId);
    }
    const oldVersion = entity.version ?? 0;
    let isUpdate = oldVersion !== 0;
    const isHistory = entity.dataType === HISTORY_TYPE || entity.dataType === HISTORY_DELETE_DIRECTIVE_TYPE;
    entity.version = entity.mtime;
    if (entity.dataType === HISTORY_TYPE) {
      isUpdate = this.sql.exec(`SELECT 1 FROM entities WHERE id = ?`, entity.id).toArray().length > 0;
    }
    const overQuota = /* @__PURE__ */ __name((refused) => {
      state.refused |= refused;
      r.responseType = ResponseType.OVER_QUOTA;
      r.errorMessage = "The chain is over its storage quota";
    }, "overQuota");
    if (isUpdate) {
      let result;
      try {
        result = this.updateEntity(entity, oldVersion);
      } catch (e) {
        if (e instanceof QuotaExceeded) return overQuota(QUOTA_BYTES);
        r.responseType = ResponseType.TRANSIENT_ERROR;
        r.errorMessage = `Update sync entity failed: ${e instanceof Error ? e.message : e}`;
        return;
      }
      if (result.conflict) {
        r.responseType = ResponseType.CONFLICT;
        return;
      }
      if (result.deleted) {
        if (isHistory) state.newHistory--;
        else state.newNormal--;
      }
    } else {
      const total = state.currentNormal + state.currentHistory + state.newNormal + state.newHistory;
      if (total >= this.cfg.maxEntities + state.boosted) {
        state.refused |= QUOTA_ENTITIES;
        r.responseType = ResponseType.OVER_QUOTA;
        r.errorMessage = `There are already ${state.currentNormal + state.currentHistory} non-deleted objects in store`;
        return;
      }
      if (isHistory && state.currentHistory + state.newHistory >= this.cfg.maxHistory) {
        state.refused |= QUOTA_HISTORY;
      } else {
        try {
          this.insertEntity(entity);
        } catch (e) {
          if (e instanceof QuotaExceeded) return overQuota(QUOTA_BYTES);
          r.responseType = e instanceof InsertError && e.conflict ? ResponseType.CONFLICT : ResponseType.TRANSIENT_ERROR;
          r.errorMessage = `Insert sync entity failed: ${e instanceof Error ? e.message : e}`;
          return;
        }
        if (entity.originatorClientItemId !== void 0) idMap.set(entity.originatorClientItemId, entity.id);
        state.passed |= isHistory ? QUOTA_HISTORY : QUOTA_ENTITIES | QUOTA_BYTES;
        if (!entity.replacedLive) {
          if (isHistory) state.newHistory++;
          else state.newNormal++;
        }
      }
    }
    r.responseType = ResponseType.SUCCESS;
    r.idString = entity.id;
    r.version = entity.version;
    r.mtime = entity.mtime;
  }
  // toDBEntity converts a committed entity (datastore.CreateDBSyncEntity).
  toDBEntity(e, cacheGuid) {
    if (e.idString === void 0) throw new Error("validate SyncEntity error: empty IdString");
    if (e.version === void 0) throw new Error("validate SyncEntity error: empty Version");
    if (e.specifics === void 0) throw new Error("validate SyncEntity error: nil Specifics");
    let dataType;
    try {
      dataType = dataTypeOfSpecifics(e.specifics);
    } catch (err) {
      throw new Error(`error finding data type to create DB sync entity: ${err instanceof ProtoError ? err.message : err}`);
    }
    let id = e.idString;
    let originatorCacheGuid;
    let originatorClientItemId;
    if (cacheGuid !== void 0) {
      if (e.version === 0) id = crypto.randomUUID();
      originatorCacheGuid = cacheGuid;
      originatorClientItemId = e.idString;
    }
    if (dataType === HISTORY_TYPE) {
      if (e.clientTagHash === void 0) throw new Error("history entity without a client tag");
      id = e.clientTagHash;
    }
    let expirationTime;
    if (dataType === HISTORY_TYPE || dataType === HISTORY_DELETE_DIRECTIVE_TYPE) {
      expirationTime = Math.floor(Date.now() / 1e3) + this.historySecs;
    }
    const mtime = this.nextMtime();
    let deleted = e.deleted;
    let folder = e.folder;
    if (e.version === 0) {
      deleted ??= false;
      folder ??= false;
    }
    return {
      id,
      parentId: e.parentIdString,
      version: e.version,
      mtime,
      ctime: e.ctime ?? mtime,
      name: e.name,
      nonUniqueName: e.nonUniqueName,
      serverDefinedUniqueTag: e.serverDefinedUniqueTag,
      deleted,
      originatorCacheGuid,
      originatorClientItemId,
      specifics: e.specifics,
      dataType,
      folder,
      clientTag: e.clientTagHash,
      uniquePosition: e.uniquePosition,
      expirationTime
    };
  }
  // addBytes adds to the chain's stored bytes, refusing growth past the cap.
  addBytes(delta) {
    if (delta > 0 && this.cfg.maxBytes > 0) {
      const current = Number(this.sql.exec(`SELECT bytes FROM chain`).one().bytes);
      if (current + delta > this.cfg.maxBytes) throw new QuotaExceeded("chain storage quota exceeded");
    }
    if (delta !== 0) this.sql.exec(`UPDATE chain SET bytes = MAX(bytes + ?, 0)`, delta);
  }
  insertTag(id) {
    const now = Date.now();
    const c = this.sql.exec(`INSERT INTO unique_tags (id, mtime, ctime) VALUES (?, ?, ?) ON CONFLICT DO NOTHING`, id, now, now);
    return c.rowsWritten > 0;
  }
  writeNewEntity(e) {
    const size = entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName);
    const c = this.sql.exec(
      `INSERT INTO entities (${ENTITY_COLUMNS}, size)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT DO NOTHING`,
      e.id,
      e.parentId ?? null,
      e.version ?? null,
      e.mtime,
      e.ctime ?? null,
      e.name ?? null,
      e.nonUniqueName ?? null,
      e.serverDefinedUniqueTag ?? null,
      boolCol(e.deleted),
      e.originatorCacheGuid ?? null,
      e.originatorClientItemId ?? null,
      blob(e.specifics),
      e.dataType,
      boolCol(e.folder),
      e.clientTag ?? null,
      blob(e.uniquePosition),
      e.expirationTime ?? null,
      size
    );
    return c.rowsWritten > 0;
  }
  // insertEntity stores a new entity (datastore.SQLite.InsertSyncEntity). A
  // create whose client tag belongs to an entity of the same type replaces
  // it under its ID, deleted or live (create over a tombstone).
  insertEntity(e) {
    this.ctx.storage.transactionSync(() => {
      const withTag = e.clientTag !== void 0 && e.dataType !== HISTORY_TYPE;
      if (withTag) {
        const old = this.sql.exec(
          `SELECT id, deleted, size FROM entities
           WHERE client_defined_unique_tag = ? AND data_type = ?
           ORDER BY mtime DESC, rowid DESC LIMIT 1`,
          e.clientTag,
          e.dataType
        ).toArray()[0];
        if (old) {
          const oldDeleted = Number(old.deleted ?? 0) !== 0;
          if (!oldDeleted && e.deleted) {
            throw new InsertError("error inserting sync item with client tag: tag in use", true);
          }
          if (!e.deleted) {
            this.reviveEntity(e, old.id, Number(old.size));
            e.replacedLive = !oldDeleted;
            return;
          }
        }
      }
      this.addBytes(entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName));
      if (withTag && !this.insertTag("Client#" + e.clientTag)) {
        throw new InsertError("error inserting sync item with client tag: tag in use", true);
      }
      if (!this.writeNewEntity(e)) {
        throw new InsertError("error inserting sync item: ID in use", withTag);
      }
    });
  }
  reviveEntity(e, oldID, oldSize) {
    const size = entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName);
    this.addBytes(size - oldSize);
    this.sql.exec(
      `UPDATE entities SET
         parent_id = ?, version = ?, mtime = ?, ctime = ?, name = ?, non_unique_name = ?,
         server_defined_unique_tag = ?, deleted = ?, originator_cache_guid = ?,
         originator_client_item_id = ?, specifics = ?, folder = ?, unique_position = ?,
         expiration_time = ?, size = ?
       WHERE id = ?`,
      e.parentId ?? null,
      e.version ?? null,
      e.mtime,
      e.ctime ?? null,
      e.name ?? null,
      e.nonUniqueName ?? null,
      e.serverDefinedUniqueTag ?? null,
      boolCol(e.deleted),
      e.originatorCacheGuid ?? null,
      e.originatorClientItemId ?? null,
      blob(e.specifics),
      boolCol(e.folder),
      blob(e.uniquePosition),
      e.expirationTime ?? null,
      size,
      oldID
    );
    this.insertTag("Client#" + e.clientTag);
    e.id = oldID;
  }
  // updateEntity updates an entity at oldVersion (any version for history)
  // (datastore.SQLite.UpdateSyncEntity).
  updateEntity(e, oldVersion) {
    const old = this.sql.exec(
      `SELECT version, deleted, size, unique_position, name, non_unique_name FROM entities WHERE id = ?`,
      e.id
    ).toArray()[0];
    if (!old) {
      if (e.deleted) return { conflict: false, deleted: false };
      if (e.clientTag !== void 0 && e.dataType !== HISTORY_TYPE) {
        e.ctime ??= e.mtime;
        e.deleted ??= false;
        e.folder ??= false;
        this.insertEntity(e);
        return { conflict: false, deleted: false };
      }
      return { conflict: true, deleted: false };
    }
    if (e.dataType !== HISTORY_TYPE && (old.version === null || Number(old.version) !== oldVersion)) {
      return { conflict: true, deleted: false };
    }
    let deleted = false;
    this.ctx.storage.transactionSync(() => {
      const pos = e.uniquePosition ?? bytesOf(old.unique_position);
      const name = e.name ?? opt(old.name);
      const nun = e.nonUniqueName ?? opt(old.non_unique_name);
      const newSize = entitySize(e.specifics, pos, name, nun);
      this.addBytes(newSize - Number(old.size ?? 0));
      this.sql.exec(
        `UPDATE entities SET
           version = ?, mtime = ?, specifics = ?, size = ?,
           unique_position = COALESCE(?, unique_position),
           parent_id = COALESCE(?, parent_id),
           name = COALESCE(?, name),
           non_unique_name = COALESCE(?, non_unique_name),
           deleted = COALESCE(?, deleted),
           folder = COALESCE(?, folder),
           expiration_time = COALESCE(?, expiration_time)
         WHERE id = ?`,
        e.version ?? null,
        e.mtime,
        blob(e.specifics),
        newSize,
        blob(e.uniquePosition),
        e.parentId ?? null,
        e.name ?? null,
        e.nonUniqueName ?? null,
        boolCol(e.deleted),
        boolCol(e.folder),
        e.expirationTime ?? null,
        e.id
      );
      const oldDeleted = old.deleted === null ? void 0 : Number(old.deleted) !== 0;
      const tagged = e.clientTag !== void 0 && e.dataType !== HISTORY_TYPE;
      if (e.deleted === true && tagged) {
        this.sql.exec(`DELETE FROM unique_tags WHERE id = ?`, "Client#" + e.clientTag);
        deleted = true;
      } else {
        if (oldDeleted === true && e.deleted === false && tagged) {
          this.insertTag("Client#" + e.clientTag);
        }
        if (e.deleted === void 0) deleted = false;
        else if (oldDeleted === void 0) deleted = e.deleted;
        else deleted = !oldDeleted && e.deleted;
      }
    });
    return { conflict: false, deleted };
  }
  // --- Counts and quota ------------------------------------------------------
  readCounts() {
    const r = this.sql.exec(
      `SELECT item_count, history_period1, history_period2, history_period3, history_period4,
              last_period_change_time FROM chain`
    ).one();
    const c = {
      itemCount: Number(r.item_count),
      h1: Number(r.history_period1),
      h2: Number(r.history_period2),
      h3: Number(r.history_period3),
      h4: Number(r.history_period4),
      lastPeriodChange: Number(r.last_period_change_time)
    };
    const now = Math.floor(Date.now() / 1e3);
    if (c.lastPeriodChange === 0) {
      c.lastPeriodChange = now;
      return c;
    }
    const period = Math.max(Math.floor(this.historySecs / 4), 1);
    const changes = Math.floor((now - c.lastPeriodChange) / period);
    for (let i = 0; i < Math.min(changes, 4); i++) {
      c.h1 = c.h2;
      c.h2 = c.h3;
      c.h3 = c.h4;
      c.h4 = 0;
    }
    if (changes > 0) c.lastPeriodChange += period * changes;
    return c;
  }
  writeCounts(c) {
    this.sql.exec(
      `UPDATE chain SET item_count = ?, history_period1 = ?, history_period2 = ?,
         history_period3 = ?, history_period4 = ?, last_period_change_time = ?`,
      Math.max(c.itemCount, 0),
      c.h1,
      c.h2,
      c.h3,
      Math.max(c.h4, 0),
      c.lastPeriodChange
    );
  }
  // recordQuota remembers which limits refused this commit's entries, and
  // forgets those an entry got past (datastore.SQLite.RecordQuota).
  recordQuota(full, cleared) {
    cleared &= ~full;
    if (full) {
      this.sql.exec(
        `UPDATE chain SET quota_full = (quota_full | ?) & ~?, quota_full_time = ?`,
        full,
        cleared,
        Math.floor(Date.now() / 1e3)
      );
    } else if (cleared) {
      this.sql.exec(`UPDATE chain SET quota_full = quota_full & ~?`, cleared);
    }
  }
  // quota answers GET /v2/compass/quota (server/quota).
  quota() {
    if (this.isDisabled()) return text(403, "chain disabled");
    const c = this.readCounts();
    const r = this.sql.exec(`SELECT bytes, quota_full, quota_full_time FROM chain`).one();
    const history = c.h1 + c.h2 + c.h3 + c.h4;
    const bytes = Number(r.bytes);
    const refused = Number(r.quota_full);
    const st = {
      entities: c.itemCount + history,
      max_entities: this.cfg.maxEntities,
      bytes,
      max_bytes: this.cfg.maxBytes,
      history,
      max_history: this.cfg.maxHistory,
      history_days: this.cfg.historyDays,
      full: [],
      full_since: r.quota_full_time === null ? 0 : Number(r.quota_full_time)
    };
    if (refused & QUOTA_ENTITIES || st.max_entities > 0 && st.entities >= st.max_entities) st.full.push("entities");
    if (refused & QUOTA_BYTES || st.max_bytes > 0 && bytes + BYTES_FULL_MARGIN > st.max_bytes) st.full.push("bytes");
    if (refused & QUOTA_HISTORY || st.max_history > 0 && history >= st.max_history) st.full.push("history");
    return json(200, st);
  }
  // --- Devices ---------------------------------------------------------------
  isDeviceRemoved(cacheGuid) {
    return this.sql.exec(`SELECT 1 FROM removed_devices WHERE cache_guid = ?`, cacheGuid).toArray().length > 0;
  }
  // removeDevice answers DELETE /v2/compass/devices/{cache_guid}: the
  // device's DEVICE_INFO is deleted for everyone and the server refuses it
  // from then on (sync.md, "Identity").
  async removeDevice(url, self) {
    const raw = url.pathname.replace(/^\/devices\/?/, "");
    let guid;
    try {
      guid = decodeURIComponent(raw);
    } catch {
      return fail(400, "bad_request");
    }
    if (!guid || guid.length > MAX_CLIENT_ID || !self || self.length > MAX_CLIENT_ID) return fail(400, "bad_request");
    if (guid === self) return fail(409, "this_device");
    if (this.isDisabled()) return fail(403, "chain_disabled");
    this.sql.exec(
      `INSERT INTO removed_devices (cache_guid, removed_at) VALUES (?, ?) ON CONFLICT DO NOTHING`,
      guid,
      Math.floor(Date.now() / 1e3)
    );
    const tag = await clientTagHash(DEVICE_INFO_TYPE, "DeviceInfo_" + guid);
    const row = this.sql.exec(
      `SELECT id, version, deleted FROM entities
       WHERE client_defined_unique_tag = ? AND data_type = ?
       ORDER BY mtime DESC, rowid DESC LIMIT 1`,
      tag,
      DEVICE_INFO_TYPE
    ).toArray()[0];
    if (row && row.version !== null && Number(row.deleted ?? 0) === 0) {
      const rsp = {};
      this.handleCommit(
        [{
          idString: row.id,
          version: Number(row.version),
          deleted: true,
          clientTagHash: tag,
          specifics: emptySpecifics(DEVICE_INFO_TYPE)
        }],
        void 0,
        rsp
      );
      const entry = rsp.commit.entryResponse[0];
      if (entry.responseType !== ResponseType.SUCCESS) {
        console.error("deleting device info", entry);
        return fail(500, "unable to complete request");
      }
    }
    for (const ws of this.ctx.getWebSockets("c:" + guid)) ws.close(1e3, "device removed");
    this.sql.exec(`DELETE FROM push_handles WHERE cache_guid = ?`, guid);
    this.notify(self, [DEVICE_INFO_TYPE]);
    return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
  }
  // --- Change notifications --------------------------------------------------
  events(request, client) {
    if (request.headers.get("Upgrade") !== "websocket") return text(426, "websocket required");
    if (this.isDisabled()) return fail(403, "sync chain disabled");
    if (client.length > MAX_CLIENT_ID) return fail(400, "client_id too long");
    if (this.ctx.getWebSockets().length >= MAX_STREAMS) {
      return fail(429, "too many streams for this chain", { "Retry-After": "60" });
    }
    const pair = new WebSocketPair();
    const [outside, inside] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(inside, client ? ["c:" + client] : []);
    inside.serializeAttachment({ client });
    return new Response(null, { status: 101, webSocket: outside });
  }
  // notify tells every stream but `except`'s which types changed.
  notify(except, types) {
    if (types.length === 0) return;
    const msg = `event: changed
data: ${JSON.stringify({ types })}

`;
    for (const ws of this.ctx.getWebSockets()) {
      const att = ws.deserializeAttachment();
      if (except && att?.client === except) continue;
      try {
        ws.send(msg);
      } catch {
      }
    }
  }
  notifyCommit(msg, rsp, isNew) {
    const entries = msg.commit?.entries ?? [];
    const results = rsp.commit?.entryResponse ?? [];
    const types = /* @__PURE__ */ new Set();
    let sentTab = false;
    entries.forEach((e, i) => {
      if (results[i]?.responseType !== ResponseType.SUCCESS || !e.specifics) return;
      try {
        const type = dataTypeOfSpecifics(e.specifics);
        types.add(type);
        if (type === SEND_TAB_TYPE && isNew[i]) sentTab = true;
      } catch {
      }
    });
    const from = msg.commit?.cacheGuid ?? "";
    this.notify(from, [...types].sort((a, b) => a - b));
    const background = [...types].some((t) => BACKGROUND_TYPES.has(t));
    if ((sentTab || background) && this.pushRelay) {
      this.ctx.waitUntil(this.pushCommit(from, sentTab, background));
    }
  }
  // --- Push (push.ts) ---------------------------------------------------------
  // The owners' choice, from the Worker with each request (access.ts),
  // kept for pushes the alarm sends. Turned off, the handles go.
  followPushChoice(header) {
    if (header !== "on" && header !== "off") return;
    const off = header === "off";
    if (off === (this.ctx.storage.kv.get("push_off") ?? false)) return;
    this.ctx.storage.kv.put("push_off", off);
    if (off) this.sql.exec(`DELETE FROM push_handles`);
  }
  get pushRelay() {
    return this.ctx.storage.kv.get("push_off") ? "" : this.cfg.pushRelay;
  }
  // register answers POST and DELETE /v2/compass/push?client_id=<cache GUID>
  // as push.go does, keeping a relay handle instead of the token. Without a
  // relay, registrations are accepted and nothing is kept.
  async register(request, guid) {
    if (this.isDisabled()) return text(403, "sync chain disabled");
    if (!guid || guid.length > MAX_CLIENT_ID) return text(400, "client_id required (the device's cache GUID)");
    if (request.method === "DELETE") {
      this.sql.exec(`DELETE FROM push_handles WHERE cache_guid = ?`, guid);
      return new Response(null, { status: 204 });
    }
    let raw;
    try {
      raw = new TextDecoder().decode(await readBody(request));
    } catch {
      return text(400, "JSON body required");
    }
    if (raw.length > 8192) return text(400, "JSON body required");
    const reg = parseRegistration(raw, this.cfg);
    if (typeof reg === "string") return text(400, reg);
    const relay = this.pushRelay;
    if (!relay) return new Response(null, { status: 204 });
    const server = request.headers.get("X-Host") ?? "";
    const handle = await seal(relay, server, reg);
    if (handle === "refused") return text(400, "the push relay refused this registration");
    if (handle === null) return text(503, "the push relay can't be reached; register again later");
    const hash = await tokenHash(reg.platform, reg.token);
    const now = Math.floor(Date.now() / 1e3);
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`DELETE FROM push_handles WHERE token_hash = ? AND cache_guid <> ?`, hash, guid);
      this.sql.exec(
        `INSERT INTO push_handles (cache_guid, handle, token_hash, platform, server, relay, updated)
         VALUES (?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (cache_guid) DO UPDATE SET handle = excluded.handle, token_hash = excluded.token_hash,
           platform = excluded.platform, server = excluded.server, relay = excluded.relay,
           updated = excluded.updated`,
        guid,
        handle,
        hash,
        reg.platform,
        server,
        relay,
        now
      );
      this.sql.exec(
        `DELETE FROM push_handles WHERE cache_guid NOT IN (
           SELECT cache_guid FROM push_handles ORDER BY updated DESC, cache_guid LIMIT ?)`,
        MAX_REGISTRATIONS
      );
    });
    return new Response(null, { status: 204 });
  }
  // The registrations made at this server's current relay (a handle from
  // another relay can't be opened by this one; its device registers again
  // at its next launch).
  handles() {
    return this.sql.exec(`SELECT cache_guid, handle, platform, server FROM push_handles WHERE relay = ?`, this.pushRelay).toArray().map((r) => ({
      guid: r.cache_guid,
      handle: r.handle,
      platform: r.platform,
      server: r.server
    }));
  }
  streaming(guid) {
    return this.ctx.getWebSockets("c:" + guid).length > 0;
  }
  // pushCommit: a new sent tab alerts every other device; a background
  // type has every other iPhone sync, spaced (push.go, OnCommit).
  async pushCommit(from, sentTab, background) {
    const now = Date.now();
    const sends = [];
    for (const h of this.handles()) {
      if (h.guid === from) continue;
      if (sentTab) sends.push(this.send(h, "send-tab"));
      if (!background || h.platform !== "apns" || this.streaming(h.guid)) continue;
      const row = this.sql.exec(`SELECT bg_last, bg_due FROM push_handles WHERE cache_guid = ?`, h.guid).one();
      if (row.bg_due !== null) continue;
      const last = Number(row.bg_last);
      if (last === 0 || now - last >= this.cfg.backgroundIntervalMs) {
        this.sql.exec(`UPDATE push_handles SET bg_last = ? WHERE cache_guid = ?`, now, h.guid);
        sends.push(this.send(h, "sync"));
      } else {
        const due = last + this.cfg.backgroundIntervalMs;
        this.sql.exec(`UPDATE push_handles SET bg_due = ? WHERE cache_guid = ?`, due, h.guid);
        const alarm = await this.ctx.storage.getAlarm();
        if (alarm === null || alarm > due) await this.ctx.storage.setAlarm(due);
      }
    }
    await Promise.all(sends);
  }
  // sendDueBackground sends the background pushes whose interval has passed
  // (from the alarm), unless Compass is open there by now.
  async sendDueBackground(now) {
    const due = this.sql.exec(`SELECT cache_guid FROM push_handles WHERE bg_due IS NOT NULL AND bg_due <= ?`, now).toArray().map((r) => r.cache_guid);
    if (due.length === 0 || !this.pushRelay) {
      this.sql.exec(`UPDATE push_handles SET bg_due = NULL WHERE bg_due IS NOT NULL AND bg_due <= ?`, now);
      return;
    }
    const byGuid = new Map(this.handles().map((h) => [h.guid, h]));
    const sends = [];
    for (const guid of due) {
      this.sql.exec(`UPDATE push_handles SET bg_due = NULL, bg_last = ? WHERE cache_guid = ?`, now, guid);
      const h = byGuid.get(guid);
      if (h && !this.streaming(guid)) sends.push(this.send(h, "sync"));
    }
    await Promise.all(sends);
  }
  async send(h, type) {
    const result = await relayPush(this.pushRelay, h.server, h.handle, type);
    if (result === "drop") {
      this.sql.exec(`DELETE FROM push_handles WHERE cache_guid = ? AND handle = ?`, h.guid, h.handle);
    }
  }
  async webSocketMessage() {
  }
  async webSocketClose(ws, code) {
    try {
      ws.close(code === 1005 ? 1e3 : code, "closed");
    } catch {
    }
  }
  // --- Rooms (calls from the Worker and room objects) -----------------------
  // roomGate is the Worker's check before a rooms request (rooms.ts): is the
  // device removed, is the chain disabled. A chain that passes is marked
  // active, as a sync request does.
  async roomGate(chainID, client) {
    this.rememberChainID(chainID);
    const removed = client !== "" && this.isDeviceRemoved(client);
    const disabled = this.isDisabled();
    if (!removed && !disabled) await this.touch();
    return { disabled, removed };
  }
  status() {
    return { disabled: this.isDisabled() };
  }
  // addRoomBytes counts a change of a room's size (it owns the room) in the
  // chain's bytes: false, and nothing counted, when growth wouldn't fit
  // COMPASS_SYNC_MAX_BYTES. Shrinking always works.
  addRoomBytes(delta) {
    try {
      this.addBytes(delta);
      return true;
    } catch (e) {
      if (e instanceof QuotaExceeded) return false;
      throw e;
    }
  }
  // roomEvent is a room's object telling of a change: the chain's index of
  // rooms takes the membership as it now is (none: not a member any more),
  // and every stream but the calling device's gets a "room" event.
  roomEvent(ev) {
    if (this.sql.exec(`SELECT chain_id FROM chain`).one().chain_id === null) this.rememberChainID(ev.chain);
    if (!this.isDisabled()) {
      this.ctx.storage.transactionSync(() => {
        if (ev.identity) this.storeIdentity(ev.identity);
        const m = ev.membership;
        if (m) {
          this.sql.exec(
            `INSERT INTO room_index (room_id, kind, owner, role, pending, epoch, seq) VALUES (?, ?, ?, ?, ?, ?, ?)
             ON CONFLICT (room_id) DO UPDATE SET kind = excluded.kind, owner = excluded.owner,
               role = excluded.role, pending = excluded.pending, epoch = excluded.epoch, seq = excluded.seq`,
            ev.room,
            m.kind,
            m.owner,
            m.role,
            m.pending ? 1 : 0,
            m.epoch,
            m.seq
          );
        } else {
          this.sql.exec(`DELETE FROM room_index WHERE room_id = ?`, ev.room);
        }
      });
    }
    if (ev.silent) return;
    const msg = `event: room
data: ${JSON.stringify({ room_id: ev.room, seq: ev.seq, changes: ev.changes })}

`;
    for (const ws of this.ctx.getWebSockets()) {
      const att = ws.deserializeAttachment();
      if (ev.skipClient && att?.client === ev.skipClient) continue;
      try {
        ws.send(msg);
      } catch {
      }
    }
  }
  // listRooms answers GET /v2/compass/rooms: the chain's current
  // memberships, by room ID; null for a chain the private server doesn't
  // list and that is in no room.
  listRooms(listed) {
    const rooms2 = this.sql.exec(`SELECT room_id, kind, owner, role, pending, epoch, seq FROM room_index ORDER BY room_id`).toArray().map((r) => ({
      id: r.room_id,
      kind: r.kind,
      owner: r.owner,
      role: r.role,
      pending: Number(r.pending) !== 0,
      epoch: Number(r.epoch),
      seq: Number(r.seq)
    }));
    if (!listed && rooms2.length === 0) return null;
    return { rooms: rooms2 };
  }
  hasRooms() {
    return this.sql.exec(`SELECT 1 FROM room_index LIMIT 1`).toArray().length > 0;
  }
  ownedRoomCount() {
    return Number(this.sql.exec(`SELECT COUNT(*) AS n FROM room_index WHERE role = 'owner'`).one().n);
  }
  // identityKey is the chain's X25519 identity key (base64), or null.
  identityKey() {
    const r = this.sql.exec(`SELECT pub FROM identity`).toArray()[0];
    return r ? r.pub : null;
  }
  // setIdentityKey records the identity key and returns the rooms the chain
  // is a current member of, whose memberships take it too.
  setIdentityKey(pub) {
    this.storeIdentity(pub);
    return this.sql.exec(`SELECT room_id FROM room_index ORDER BY room_id`).toArray().map((r) => r.room_id);
  }
  storeIdentity(pub) {
    this.sql.exec(
      `INSERT INTO identity (one, pub, updated) VALUES (1, ?, ?)
       ON CONFLICT (one) DO UPDATE SET pub = excluded.pub, updated = excluded.updated WHERE identity.pub IS NOT excluded.pub`,
      pub,
      Date.now()
    );
  }
  // --- Maintenance -----------------------------------------------------------
  // disable clears the chain's data and refuses it from now on ("reset
  // sync", and idle chains): ClearServerData. Its rooms go too (sync.md,
  // "Going away"): those it owns are deleted, its other memberships and
  // envelopes and its identity key go; nothing is announced.
  async disable() {
    const chain = String(this.sql.exec(`SELECT chain_id FROM chain`).one().chain_id ?? "").toLowerCase();
    const rooms2 = this.sql.exec(`SELECT room_id FROM room_index`).toArray().map((r) => r.room_id);
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`DELETE FROM entities`);
      this.sql.exec(`DELETE FROM unique_tags`);
      this.sql.exec(`DELETE FROM removed_devices`);
      this.sql.exec(`DELETE FROM room_index`);
      this.sql.exec(`DELETE FROM identity`);
      this.sql.exec(`DELETE FROM push_handles`);
      this.sql.exec(
        `UPDATE chain SET disabled_at = ?, bytes = 0, item_count = 0, history_period1 = 0,
           history_period2 = 0, history_period3 = 0, history_period4 = 0, quota_full = 0`,
        Date.now()
      );
    });
    for (const ws of this.ctx.getWebSockets()) ws.close(1e3, "chain disabled");
    if (!chain) return;
    for (let i = 0; i < rooms2.length; i += 50) {
      await Promise.all(
        rooms2.slice(i, i + 50).map(
          (room2) => this.env.ROOM.get(this.env.ROOM.idFromName(room2)).chainGone(room2, chain).catch((e) => console.error("clearing a room of a cleared chain failed", e))
        )
      );
    }
  }
  // alarm runs hourly while the chain is in use: expired history and old
  // tombstones go, and an idle chain is cleared and disabled. It also sends
  // background pushes that waited for their interval (pushCommit), so it
  // runs at the earlier of the two.
  async alarm() {
    if (this.isDisabled()) return;
    const now = Date.now();
    await this.sendDueBackground(now);
    const purgeAt = this.ctx.storage.kv.get("purge_at") ?? 0;
    if (now < purgeAt) {
      await this.setNextAlarm(purgeAt);
      return;
    }
    const lastActive = this.sql.exec(`SELECT last_active FROM chain`).one().last_active;
    if (this.cfg.idleChainDays > 0 && lastActive !== null && Number(lastActive) < Math.floor(now / 1e3) - this.cfg.idleChainDays * 86400) {
      await this.disable();
      return;
    }
    this.purge(now);
    this.ctx.storage.kv.put("purge_at", now + PURGE_INTERVAL_MS);
    await this.setNextAlarm(now + PURGE_INTERVAL_MS);
  }
  async setNextAlarm(purgeAt) {
    const due = this.sql.exec(`SELECT MIN(bg_due) AS d FROM push_handles`).one().d;
    await this.ctx.storage.setAlarm(due === null ? purgeAt : Math.min(purgeAt, Number(due)));
  }
  purge(now) {
    const cutoff = Math.floor(now / 1e3);
    this.ctx.storage.transactionSync(() => {
      const expired = `expiration_time > 0 AND expiration_time < ?`;
      this.sql.exec(
        `DELETE FROM unique_tags WHERE id IN (
           SELECT 'Client#' || client_defined_unique_tag FROM entities
           WHERE ${expired} AND client_defined_unique_tag IS NOT NULL AND data_type != ?)`,
        cutoff,
        HISTORY_TYPE
      );
      const gone = Number(
        this.sql.exec(`SELECT COALESCE(SUM(size), 0) AS s FROM entities WHERE ${expired}`, cutoff).one().s
      );
      this.sql.exec(`DELETE FROM entities WHERE ${expired}`, cutoff);
      if (this.cfg.tombstoneDays > 0) {
        const before = now - this.cfg.tombstoneDays * 864e5;
        const tomb = Number(
          this.sql.exec(`SELECT COALESCE(SUM(size), 0) AS s FROM entities WHERE deleted = 1 AND mtime < ?`, before).one().s
        );
        this.sql.exec(`DELETE FROM entities WHERE deleted = 1 AND mtime < ?`, before);
        this.sql.exec(`UPDATE chain SET bytes = MAX(0, bytes - ?)`, gone + tomb);
      } else {
        this.sql.exec(`UPDATE chain SET bytes = MAX(0, bytes - ?)`, gone);
      }
    });
  }
};
var CodedError = class extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
  code;
  static {
    __name(this, "CodedError");
  }
};
function protoResponse(rsp) {
  return new Response(encodeClientToServerResponse(rsp), {
    status: 200,
    headers: { "Content-Type": "application/x-protobuf" }
  });
}
__name(protoResponse, "protoResponse");
async function readBody(request) {
  let stream = request.body;
  if (!stream) return new Uint8Array(0);
  if (request.headers.get("Content-Encoding") === "gzip") {
    stream = stream.pipeThrough(new DecompressionStream("gzip"));
  }
  const reader = stream.getReader();
  const parts = [];
  let size = 0;
  for (; ; ) {
    const { done, value } = await reader.read();
    if (done) break;
    const take = Math.min(value.length, PAYLOAD_LIMIT - size);
    if (take > 0) parts.push(value.subarray(0, take));
    size += take;
    if (size >= PAYLOAD_LIMIT) {
      await reader.cancel();
      break;
    }
  }
  const out = new Uint8Array(size);
  let pos = 0;
  for (const p of parts) {
    out.set(p, pos);
    pos += p.length;
  }
  return out;
}
__name(readBody, "readBody");

// src/pairing.ts
import { DurableObject as DurableObject3 } from "cloudflare:workers";
var SLOT_TTL_MS = 3e5;
var POLL_TIMEOUT_MS = 25e3;
var MAX_MESSAGES_PER_SIDE = 8;
var MAX_MESSAGE_BYTES = 4096;
var JOINS_PER_IP = 10;
var OPENS_PER_IP = 30;
var WINDOW_MS = 6e5;
var MAX_OPEN_SLOTS = 1e4;
var CLOSED_TTL_MS = 3e5;
var PAIR_TOKEN_HEADER = "X-Compass-Pair-Token";
var Pairing = class extends DurableObject3 {
  static {
    __name(this, "Pairing");
  }
  slots = /* @__PURE__ */ new Map();
  closed = /* @__PURE__ */ new Map();
  byChain = /* @__PURE__ */ new Map();
  joins = /* @__PURE__ */ new Map();
  opens = /* @__PURE__ */ new Map();
  lastSweep = 0;
  async fetch(request) {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    const m = request.method;
    if (parts.length === 0 && m === "POST") return this.open(request);
    if (parts.length === 0) return fail(405, "method not allowed");
    const num2 = /^\d+$/.test(parts[0]) ? Number(parts[0]) : 0;
    if (parts.length === 2 && parts[1] === "join" && m === "POST") return this.join(request, num2);
    if (parts.length === 2 && parts[1] === "msg" && m === "POST") return this.post(request, num2);
    if (parts.length === 2 && parts[1] === "msg" && m === "GET") return this.poll(request, url, num2);
    if (parts.length === 1 && m === "DELETE") return this.close(request, url, num2);
    return fail(404, "not found");
  }
  closeSlot(sl, reason, at) {
    this.slots.delete(sl.num);
    this.closed.set(sl.num, { reason, until: at + CLOSED_TTL_MS });
    if (this.byChain.get(sl.chain) === sl.num) this.byChain.delete(sl.chain);
    for (const w of sl.waiters) w();
    sl.waiters.clear();
  }
  lookup(num2, now) {
    let sl = this.slots.get(num2);
    if (sl && now >= sl.expires) {
      this.closeSlot(sl, "expired", sl.expires);
      sl = void 0;
    }
    if (sl) return { sl };
    const c = this.closed.get(num2);
    if (!c) return {};
    if (now >= c.until) {
      this.closed.delete(num2);
      return {};
    }
    return { reason: c.reason };
  }
  sweep(now) {
    if (now - this.lastSweep < 6e4) return;
    this.lastSweep = now;
    for (const sl of [...this.slots.values()]) if (now >= sl.expires) this.closeSlot(sl, "expired", sl.expires);
    for (const [n, c] of this.closed) if (now >= c.until) this.closed.delete(n);
    for (const m of [this.joins, this.opens]) {
      for (const [ip, times] of m) {
        const kept = times.filter((t) => t > now - WINDOW_MS);
        if (kept.length) m.set(ip, kept);
        else m.delete(ip);
      }
    }
  }
  allow(m, ip, limit, now) {
    const times = (m.get(ip) ?? []).filter((t) => t > now - WINDOW_MS);
    if (times.length >= limit) {
      m.set(ip, times);
      return false;
    }
    times.push(now);
    m.set(ip, times);
    return true;
  }
  canonicalHost(request) {
    let h = this.env.COMPASS_SYNC_PUBLIC_HOST || request.headers.get("X-Host") || "";
    if (h.startsWith("[")) h = h.slice(0, h.indexOf("]") + 1);
    else if (h.split(":").length === 2) h = h.split(":")[0];
    return h.toLowerCase();
  }
  open(request) {
    const chain = (request.headers.get("X-Chain-ID") ?? "").toLowerCase();
    if (!chain) return fail(401, "invalid sync token");
    const ip = request.headers.get("X-Client-IP") ?? "";
    const now = Date.now();
    this.sweep(now);
    if (!this.allow(this.opens, ip, OPENS_PER_IP, now)) return fail(429, "too many pairing slots opened");
    const oldNum = this.byChain.get(chain);
    const old = oldNum === void 0 ? void 0 : this.slots.get(oldNum);
    if (old && old.chain === chain) {
      if (now >= old.expires) this.closeSlot(old, "expired", old.expires);
      else this.closeSlot(old, "cancel", now);
    }
    if (this.slots.size >= MAX_OPEN_SLOTS) return fail(503, "too many open pairing slots");
    let num2 = 1;
    for (; ; ) {
      const { sl: sl2, reason } = this.lookup(num2, now);
      if (!sl2 && !reason) break;
      num2++;
    }
    const sl = {
      num: num2,
      chain,
      server: this.canonicalHost(request),
      expires: now + SLOT_TTL_MS,
      bToken: "",
      msgs: [[], []],
      waiters: /* @__PURE__ */ new Set()
    };
    this.slots.set(num2, sl);
    this.byChain.set(chain, num2);
    return json(201, { slot: num2, expires_in: SLOT_TTL_MS / 1e3, server: sl.server });
  }
  gone(reason) {
    if (reason) return json(410, { error: "pairing slot closed", reason });
    return fail(404, "no such pairing slot");
  }
  join(request, num2) {
    const ip = request.headers.get("X-Client-IP") ?? "";
    const now = Date.now();
    this.sweep(now);
    if (!this.allow(this.joins, ip, JOINS_PER_IP, now)) return fail(429, "too many join attempts");
    const { sl, reason } = num2 >= 1 ? this.lookup(num2, now) : {};
    if (!sl) return this.gone(reason);
    if (sl.bToken) return fail(409, "pairing slot already joined");
    const b = crypto.getRandomValues(new Uint8Array(16));
    sl.bToken = Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
    return json(200, {
      b_token: sl.bToken,
      server: sl.server,
      expires_in: Math.ceil((sl.expires - now) / 1e3)
    });
  }
  // authorize: 0 if the caller may act as `side` of the slot, else a status.
  authorize(sl, side, request) {
    if (side === 0) {
      const chain = (request.headers.get("X-Chain-ID") ?? "").toLowerCase();
      if (!chain) return 401;
      return chain === sl.chain ? 0 : 403;
    }
    const t = request.headers.get(PAIR_TOKEN_HEADER) ?? "";
    if (!t) return 401;
    if (!sl.bToken || !timingSafeEqual(t, sl.bToken)) return 403;
    return 0;
  }
  async post(request, num2) {
    if (num2 < 1) return fail(404, "no such pairing slot");
    const limit = Math.ceil(MAX_MESSAGE_BYTES / 3) * 4 + 1024;
    const raw = await request.text();
    if (raw.length > limit) return fail(413, "message too large");
    let body;
    try {
      body = JSON.parse(raw);
    } catch {
      return fail(400, "body must be JSON {from, data}");
    }
    const sideN = body.from === "a" ? 0 : body.from === "b" ? 1 : -1;
    if (sideN < 0) return fail(400, `"from" must be "a" or "b"`);
    const side = sideN;
    let data;
    try {
      if (typeof body.data !== "string") throw new Error();
      data = Uint8Array.from(atob(body.data), (c) => c.charCodeAt(0));
    } catch {
      return fail(400, `"data" must be non-empty standard base64`);
    }
    if (data.length === 0) return fail(400, `"data" must be non-empty standard base64`);
    if (data.length > MAX_MESSAGE_BYTES) return fail(413, "message too large");
    const { sl, reason } = this.lookup(num2, Date.now());
    if (!sl) return this.gone(reason);
    const status = this.authorize(sl, side, request);
    if (status) return fail(status, `not allowed to post as ${body.from}`);
    if (sl.msgs[side].length >= MAX_MESSAGES_PER_SIDE) return fail(429, "too many messages");
    sl.msgs[side].push(btoa(String.fromCharCode(...data)));
    for (const w of sl.waiters) w();
    sl.waiters.clear();
    return json(200, { seq: sl.msgs[side].length });
  }
  async poll(request, url, num2) {
    if (num2 < 1) return fail(404, "no such pairing slot");
    const forSide = url.searchParams.get("for");
    const reader = forSide === "a" ? 0 : forSide === "b" ? 1 : -1;
    if (reader < 0) return fail(400, `"for" must be "a" or "b"`);
    let after = 0;
    const a = url.searchParams.get("after");
    if (a !== null && a !== "") {
      if (!/^\d+$/.test(a)) return fail(400, `"after" must be a sequence number`);
      after = Number(a);
    }
    const sender = reader === 1 ? 0 : 1;
    const deadline = Date.now() + POLL_TIMEOUT_MS;
    for (; ; ) {
      const now = Date.now();
      const { sl, reason } = this.lookup(num2, now);
      if (!sl) return this.gone(reason);
      const status = this.authorize(sl, reader, request);
      if (status) return fail(status, "not allowed to read");
      const msgs = sl.msgs[sender].slice(after).map((data, i) => ({ seq: after + i + 1, data }));
      if (msgs.length > 0) return json(200, { messages: msgs });
      if (now >= deadline) return json(200, { messages: [] });
      const wait = Math.min(deadline - now, sl.expires - now);
      await new Promise((resolve) => {
        const done = /* @__PURE__ */ __name(() => {
          clearTimeout(timer);
          sl.waiters.delete(done);
          resolve();
        }, "done");
        const timer = setTimeout(done, Math.max(wait, 0));
        sl.waiters.add(done);
        request.signal?.addEventListener?.("abort", done);
      });
      if (request.signal?.aborted) return json(200, { messages: [] });
    }
  }
  close(request, url, num2) {
    if (num2 < 1) return fail(404, "no such pairing slot");
    let why = url.searchParams.get("reason") ?? "";
    if (why === "") why = "cancel";
    if (why !== "mismatch" && why !== "cancel" && why !== "deny") {
      return fail(400, `"reason" must be "mismatch", "cancel" or "deny"`);
    }
    const now = Date.now();
    const { sl, reason } = this.lookup(num2, now);
    if (!sl) return this.gone(reason);
    const sa = this.authorize(sl, 0, request);
    const sb = this.authorize(sl, 1, request);
    if (sa && sb) return fail(Math.max(sa, sb), "not allowed to close");
    this.closeSlot(sl, why, now);
    return new Response(null, { status: 204 });
  }
};
function timingSafeEqual(a, b) {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}
__name(timingSafeEqual, "timingSafeEqual");

// src/room.ts
import { DurableObject as DurableObject4 } from "cloudflare:workers";
var MAX_WATCHERS = 1e3;
var SCHEMA3 = `
CREATE TABLE IF NOT EXISTS room (
  one     INTEGER PRIMARY KEY CHECK (one = 1),
  id      TEXT NOT NULL,
  owner   TEXT NOT NULL,
  kind    TEXT NOT NULL,
  created INTEGER NOT NULL,
  seq     INTEGER NOT NULL DEFAULT 0,
  epoch   INTEGER NOT NULL DEFAULT 1,
  bytes   INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS members (
  client_id    TEXT PRIMARY KEY,
  role         TEXT NOT NULL,
  identity_pub BLOB,
  profile      BLOB,
  invited      INTEGER NOT NULL,
  joined       INTEGER NOT NULL DEFAULT 0,
  removed      INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS records (
  record_id  TEXT PRIMARY KEY,
  seq        INTEGER NOT NULL,
  epoch      INTEGER NOT NULL,
  deleted    INTEGER NOT NULL DEFAULT 0,
  author     TEXT NOT NULL,
  ciphertext BLOB,
  size       INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS records_by_seq ON records (seq);
CREATE TABLE IF NOT EXISTS envelopes (
  client_id TEXT NOT NULL,
  epoch     INTEGER NOT NULL,
  sealed    BLOB NOT NULL,
  PRIMARY KEY (client_id, epoch)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS invites (
  id          TEXT PRIMARY KEY,
  secret_hash BLOB NOT NULL,
  role        TEXT NOT NULL,
  created     INTEGER NOT NULL,
  expires     INTEGER NOT NULL DEFAULT 0,
  uses_left   INTEGER,
  revoked     INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
`;
function bytesOf2(v) {
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (v instanceof Uint8Array) return v;
  return null;
}
__name(bytesOf2, "bytesOf");
function blob2(b) {
  if (!b || b.length === 0) return null;
  return b.slice().buffer;
}
__name(blob2, "blob");
function b64OrNull(b) {
  return b === null ? null : b64(b);
}
__name(b64OrNull, "b64OrNull");
function validRole(role) {
  return role === ROLE_EDITOR || role === ROLE_VIEWER;
}
__name(validRole, "validRole");
function chainStub2(env, chain) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}
__name(chainStub2, "chainStub");
function recordOut(r) {
  const out = {
    id: r.record_id,
    seq: Number(r.seq),
    epoch: Number(r.epoch),
    deleted: Number(r.deleted) !== 0,
    author: r.author
  };
  const ct = bytesOf2(r.ciphertext);
  if (ct && ct.length > 0) out.ciphertext = b64(ct);
  return out;
}
__name(recordOut, "recordOut");
function parseCount(v, def) {
  if (v === null || v === "") return def;
  if (!/^[+-]?\d+$/.test(v)) return null;
  const n = Number(v);
  return Number.isSafeInteger(n) ? n : null;
}
__name(parseCount, "parseCount");
var quotaExceeded = /* @__PURE__ */ __name(() => new HttpError(507, "quota_exceeded"), "quotaExceeded");
var Room = class extends DurableObject4 {
  static {
    __name(this, "Room");
  }
  sql;
  queue = Promise.resolve();
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(SCHEMA3);
  }
  // locked runs the room's changes one at a time: they await other objects
  // (quota, events) between reading and writing.
  locked(fn) {
    const p = this.queue.then(fn, fn);
    this.queue = p.catch(() => {
    });
    return p;
  }
  // --- HTTP ------------------------------------------------------------------
  async fetch(request) {
    const url = new URL(request.url);
    const h = request.headers;
    const c = {
      id: (h.get("X-Chain-ID") ?? "").toLowerCase(),
      listed: h.get("X-Listed") === "1",
      device: h.get("X-Client-ID") ?? "",
      room: h.get("X-Room-ID") ?? ""
    };
    if (!c.id || !c.room) return text(401, "Unauthorized");
    const path = url.pathname;
    const method = request.method;
    try {
      if (path === "/create" && method === "POST") return await this.create(c, request);
      if (path === "/join" && method === "POST") return await this.join(c, request);
      const handler = this.route(method, path);
      if (!handler) return text(404, "404 page not found");
      if (!c.listed && !this.member(c.id)) throw new HttpError(403, "chain_not_allowed");
      return await handler(c, request, url);
    } catch (e) {
      return errorResponse(e);
    }
  }
  route(method, path) {
    const seg = path.split("/").filter(Boolean);
    const [a, b] = seg;
    const is = /* @__PURE__ */ __name((m) => method === m, "is");
    if (seg.length === 0) {
      if (is("GET")) return (c) => this.view(c, 200);
      if (is("DELETE")) return (c) => this.deleteRoom(c);
      return null;
    }
    if (seg.length === 1) {
      if (a === "changes" && is("GET")) return (c, r, u) => this.changes(c, r, u);
      if (a === "records" && is("POST")) return (c, r) => this.records(c, r);
      if (a === "invites" && is("POST")) return (c, r) => this.createInvite(c, r);
      if (a === "members" && is("POST")) return () => {
        throw new HttpError(404, "directory_disabled");
      };
      if (a === "owner" && is("POST")) return (c, r) => this.transfer(c, r);
      if (a === "successor" && is("POST")) return (c, r) => this.successor(c, r);
      if (a === "epochs" && is("POST")) return (c, r) => this.rotate(c, r);
      if (a === "envelopes" && is("POST")) return (c, r) => this.addEnvelopes(c, r);
      return null;
    }
    if (seg.length === 2) {
      if (a === "invites" && is("DELETE")) return (c) => this.revokeInvite(c, b);
      if (a === "members" && is("PATCH")) return (c, r) => this.updateMember(c, r, b);
      if (a === "members" && is("DELETE")) return (c) => this.removeMember(c, b);
    }
    return null;
  }
  // --- State -----------------------------------------------------------------
  roomRow() {
    const r = this.sql.exec(`SELECT id, owner, kind, created, seq, epoch, bytes FROM room`).toArray()[0];
    if (!r) return null;
    return {
      id: r.id,
      owner: r.owner,
      kind: r.kind,
      created: Number(r.created),
      seq: Number(r.seq),
      epoch: Number(r.epoch),
      bytes: Number(r.bytes)
    };
  }
  memberOf(r) {
    return {
      client_id: r.client_id,
      role: r.role,
      identity_pub: bytesOf2(r.identity_pub),
      profile: bytesOf2(r.profile),
      invited: Number(r.invited),
      joined: Number(r.joined),
      removed: Number(r.removed)
    };
  }
  member(chain) {
    const r = this.sql.exec(`SELECT * FROM members WHERE client_id = ?`, chain.toLowerCase()).toArray()[0];
    return r ? this.memberOf(r) : null;
  }
  // currentMembers are the joined and pending members, by client ID.
  currentMembers() {
    return this.sql.exec(`SELECT * FROM members WHERE removed = 0 ORDER BY client_id`).toArray().map((r) => this.memberOf(r));
  }
  currentIDs() {
    return this.currentMembers().map((m) => m.client_id);
  }
  // access loads the room and the caller's membership: not_found if either
  // is missing, removed, not_joined if pending and !pendingOK, forbidden if
  // its role isn't in roles (none: any).
  access(caller, pendingOK, roles = []) {
    const room2 = this.roomRow();
    if (!room2) throw new HttpError(404, "not_found");
    const me = this.member(caller);
    if (!me) throw new HttpError(404, "not_found");
    if (me.removed !== 0) throw new HttpError(403, "removed");
    if (me.joined === 0 && !pendingOK) throw new HttpError(403, "not_joined");
    if (roles.length > 0 && !roles.includes(me.role)) throw new HttpError(403, "forbidden");
    return { room: room2, me };
  }
  // currentTarget is a member operation's target: member_not_found unless
  // it's a current member.
  currentTarget(chain) {
    const m = this.member(chain);
    if (!m || m.removed !== 0) throw new HttpError(404, "member_not_found");
    return m;
  }
  putEnvelope(chain, epoch, sealed) {
    this.sql.exec(
      `INSERT INTO envelopes (client_id, epoch, sealed) VALUES (?, ?, ?)
       ON CONFLICT (client_id, epoch) DO UPDATE SET sealed = excluded.sealed`,
      chain,
      epoch,
      blob2(sealed)
    );
  }
  wipe() {
    this.ctx.storage.transactionSync(() => {
      for (const t of ["room", "members", "records", "envelopes", "invites"]) this.sql.exec(`DELETE FROM ${t}`);
    });
  }
  // --- Notifications ---------------------------------------------------------
  membership(chain, room2) {
    if (!room2) return null;
    const m = this.member(chain);
    if (!m || m.removed !== 0) return null;
    return { kind: room2.kind, owner: room2.owner, role: m.role, pending: m.joined === 0, epoch: room2.epoch, seq: room2.seq };
  }
  // announce tells each chain of targets of a change to the room: a "room"
  // event on its streams (but the calling device's) and its membership as
  // it now is, for its index. Failures are logged: a member finds out at
  // its next request.
  async announce(c, change, targets, opts = {}) {
    const room2 = this.roomRow();
    const seq = opts.seq ?? room2?.seq ?? 0;
    await Promise.all(
      [...new Set(targets)].map(async (t) => {
        const ev = {
          chain: t,
          room: c.room,
          seq,
          changes: [change],
          skipClient: t === c.id ? c.device : "",
          membership: this.membership(t, room2)
        };
        if (opts.silent) ev.silent = true;
        if (opts.identity && opts.identity.chain === t) ev.identity = b64(opts.identity.pub);
        try {
          await chainStub2(this.env, t).roomEvent(ev);
        } catch (e) {
          console.error("rooms: telling a member failed", e);
        }
      })
    );
  }
  // wake ends the long polls of chain (all: every one).
  wake(chain) {
    for (const ws of this.ctx.getWebSockets(chain ? "w:" + chain : void 0)) {
      try {
        ws.send("wake");
      } catch {
      }
    }
  }
  async webSocketMessage() {
  }
  async webSocketClose(ws, code) {
    try {
      ws.close(code === 1005 ? 1e3 : code, "closed");
    } catch {
    }
  }
  // --- Answers ---------------------------------------------------------------
  // view answers the room as c sees it.
  view(c, status) {
    const { room: room2, me } = this.access(c.id, true);
    const members = this.currentMembers().map((m) => ({
      client_id: m.client_id,
      role: m.role,
      identity_pub: b64OrNull(m.identity_pub),
      profile: b64OrNull(m.profile),
      name: "",
      // No company sign-in on this server.
      email: "",
      invited: m.invited,
      joined: m.joined,
      pending: m.joined === 0
    }));
    const envelopes = this.sql.exec(`SELECT epoch, sealed FROM envelopes WHERE client_id = ? ORDER BY epoch`, me.client_id).toArray().map((r) => ({ epoch: Number(r.epoch), sealed: b64(bytesOf2(r.sealed) ?? new Uint8Array(0)) }));
    const out = {
      id: room2.id,
      kind: room2.kind,
      owner: room2.owner,
      created: room2.created,
      epoch: room2.epoch,
      seq: room2.seq,
      bytes: room2.bytes,
      role: me.role,
      pending: me.joined === 0,
      members,
      envelopes
    };
    if (me.role === ROLE_OWNER) out.invites = this.invites().map(inviteOut);
    return json(status, out);
  }
  invites() {
    return this.sql.exec(`SELECT * FROM invites ORDER BY created, id`).toArray().map(inviteRow);
  }
  // --- Routes ----------------------------------------------------------------
  // create makes the room (the Worker checked the body, the caller's right
  // to create and its number of rooms).
  async create(c, request) {
    const body = await request.json();
    const pub = decodeStd(body.identity_pub);
    const envelope = decodeStd(body.envelope);
    const profile = body.profile ? decodeStd(body.profile) : null;
    return this.locked(async () => {
      if (this.roomRow()) throw new HttpError(409, "room_exists");
      const now = Date.now();
      this.ctx.storage.transactionSync(() => {
        this.sql.exec(
          `INSERT INTO room (one, id, owner, kind, created, seq, epoch, bytes) VALUES (1, ?, ?, ?, ?, 0, 1, 0)`,
          c.room,
          c.id,
          body.kind,
          now
        );
        this.sql.exec(
          `INSERT INTO members (client_id, role, identity_pub, profile, invited, joined, removed)
           VALUES (?, ?, ?, ?, ?, ?, 0)`,
          c.id,
          ROLE_OWNER,
          blob2(pub),
          blob2(profile),
          now,
          now
        );
        this.putEnvelope(c.id, 1, envelope);
      });
      await this.announce(c, "members", [c.id], { silent: true, identity: { chain: c.id, pub } });
      return this.view(c, 201);
    });
  }
  async deleteRoom(c) {
    return this.locked(async () => {
      const { room: room2 } = this.access(c.id, false, [ROLE_OWNER]);
      const members = this.currentIDs();
      this.wipe();
      this.wake();
      if (room2.bytes !== 0) await chainStub2(this.env, room2.owner).addRoomBytes(-room2.bytes);
      await this.announce(c, "deleted", members, { seq: room2.seq });
      return noContent();
    });
  }
  // changes answers GET .../changes. A WebSocket request (the Worker's long
  // poll) with nothing new becomes a watcher: it gets {"seq","epoch"} now
  // and "wake" when it should ask again.
  changes(c, request, url) {
    const q = url.searchParams;
    const since = parseCount(q.get("since"), 0);
    if (since === null || since < 0) throw new Bad("since must be a sequence number");
    const limit = parseCount(q.get("limit"), MAX_CHANGES_LIMIT);
    if (limit === null || limit < 1 || limit > MAX_CHANGES_LIMIT) throw new Bad("limit must be 1 to 1000");
    const wait = parseCount(q.get("wait"), 0);
    if (wait === null || wait < 0 || wait > MAX_WAIT_SECONDS) throw new Bad("wait must be 0 to 30 seconds");
    const { room: room2 } = this.access(c.id, false);
    const records = [];
    let total = 0;
    let more = false;
    for (const r of this.sql.exec(
      `SELECT record_id, seq, epoch, deleted, author, ciphertext FROM records WHERE seq > ? ORDER BY seq LIMIT ?`,
      since,
      limit + 1
    )) {
      if (records.length === limit || records.length > 0 && total >= MAX_CHANGES_BYTES) {
        more = true;
        break;
      }
      total += bytesOf2(r.ciphertext)?.length ?? 0;
      records.push(recordOut(r));
    }
    const watch = request.headers.get("Upgrade") === "websocket";
    if (!watch || wait === 0 || records.length > 0 || this.ctx.getWebSockets().length >= MAX_WATCHERS) {
      return json(200, { seq: room2.seq, epoch: room2.epoch, records, more });
    }
    const pair = new WebSocketPair();
    const [outside, inside] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(inside, ["w:" + c.id]);
    inside.send(JSON.stringify({ seq: room2.seq, epoch: room2.epoch }));
    return new Response(null, { status: 101, webSocket: outside });
  }
  async records(c, request) {
    const body = await readJSONBody(request, MAX_WRITE_BYTES);
    const raw = fObjects(body, "records").map((r) => ({
      id: fStr(r, "id"),
      epoch: fInt(r, "epoch"),
      deleted: fBool(r, "deleted"),
      ciphertext: fBytes(r, "ciphertext"),
      baseSeq: fOptInt(r, "base_seq")
    }));
    if (raw.length === 0 || raw.length > MAX_RECORDS_PER_WRITE) throw new Bad("records must hold 1 to 500 records");
    const seen = /* @__PURE__ */ new Set();
    let total = 0;
    for (const r of raw) {
      const ctLen = r.ciphertext?.length ?? 0;
      if (!RECORD_ID_RE.test(r.id)) throw new Bad("record id must be 1 to 128 of A-Z a-z 0-9 . _ : -");
      if (seen.has(r.id)) throw new Bad(`record ${r.id} appears twice`);
      if (r.epoch < 1) throw new Bad("epoch must be 1 or more");
      if (!r.deleted && ctLen < RECORD_OVERHEAD) {
        throw new Bad(`record ${r.id} needs a ciphertext of 28 bytes or more (nonce, ciphertext, tag)`);
      }
      if (ctLen > MAX_ROOM_RECORD_BYTES) throw new HttpError(413, "record_too_large");
      if (r.baseSeq !== null && r.baseSeq < 0) throw new Bad("base_seq must be 0 or more");
      seen.add(r.id);
      total += ctLen;
    }
    if (total > MAX_BATCH_CIPHERTEXT) throw new HttpError(413, "too_large");
    return this.locked(async () => {
      const { room: room2, me } = this.access(c.id, false, [ROLE_OWNER, ROLE_EDITOR]);
      const conflicts = [];
      const oldSizes = [];
      for (const w of raw) {
        if (w.epoch !== room2.epoch) throw new HttpError(409, "stale_epoch");
        const old = this.sql.exec(`SELECT record_id, seq, epoch, deleted, author, ciphertext, size FROM records WHERE record_id = ?`, w.id).toArray()[0];
        if (!old) {
          oldSizes.push(0);
          if (w.baseSeq !== null && w.baseSeq !== 0) {
            conflicts.push({ id: w.id, seq: 0, epoch: 0, deleted: false, author: "" });
          }
        } else {
          oldSizes.push(Number(old.size));
          if (w.baseSeq !== null && w.baseSeq !== Number(old.seq)) conflicts.push(recordOut(old));
        }
      }
      if (conflicts.length > 0) throw new HttpError(409, "conflict", { current: conflicts });
      let delta = 0;
      raw.forEach((w, i) => {
        const ctLen = w.deleted ? 0 : w.ciphertext?.length ?? 0;
        delta += ctLen + w.id.length - oldSizes[i];
      });
      const owner = chainStub2(this.env, room2.owner);
      if (delta > 0 && !await owner.addRoomBytes(delta)) throw quotaExceeded();
      const out = [];
      let seq = room2.seq;
      try {
        this.ctx.storage.transactionSync(() => {
          for (const w of raw) {
            seq++;
            const ct = w.deleted ? null : w.ciphertext;
            const size = (ct?.length ?? 0) + w.id.length;
            this.sql.exec(
              `INSERT INTO records (record_id, seq, epoch, deleted, author, ciphertext, size)
               VALUES (?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT (record_id) DO UPDATE SET seq = excluded.seq, epoch = excluded.epoch,
                 deleted = excluded.deleted, author = excluded.author, ciphertext = excluded.ciphertext,
                 size = excluded.size`,
              w.id,
              seq,
              w.epoch,
              w.deleted ? 1 : 0,
              me.client_id,
              blob2(ct),
              size
            );
            out.push({ id: w.id, seq });
          }
          this.sql.exec(`UPDATE room SET seq = ?, bytes = MAX(bytes + ?, 0)`, seq, delta);
        });
      } catch (e) {
        if (delta > 0) await owner.addRoomBytes(-delta);
        throw e;
      }
      if (delta < 0) await owner.addRoomBytes(delta);
      this.wake();
      await this.announce(c, "records", this.currentIDs());
      return json(200, { seq, records: out });
    });
  }
  async createInvite(c, request) {
    const body = await readJSONBody(request, MAX_BODY);
    const role = fStr(body, "role");
    const secretHash = fStr(body, "secret_hash");
    const expires = fInt(body, "expires");
    const uses = fInt(body, "uses");
    const now = Date.now();
    if (!validRole(role)) throw new Bad("role must be editor or viewer");
    if (!SECRET_HASH_RE.test(secretHash)) {
      throw new Bad("secret_hash must be the SHA-256 of the secret, 64 lower-case hex digits");
    }
    if (expires < 0 || expires !== 0 && expires <= now) throw new Bad("expires must be 0 or in the future");
    if (uses < 0 || uses > MAX_INVITE_USES) throw new Bad("uses must be 0 to 1000");
    const hash = new Uint8Array(secretHash.match(/../g).map((x) => parseInt(x, 16)));
    return this.locked(async () => {
      this.access(c.id, false, [ROLE_OWNER]);
      const open = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM invites WHERE revoked = 0`).one().n);
      if (open >= MAX_ROOM_INVITES) throw new HttpError(409, "too_many_invites");
      const inv = {
        id: newID(),
        secret_hash: hash,
        role,
        created: now,
        expires,
        uses_left: uses === 0 ? null : uses,
        revoked: false
      };
      this.sql.exec(
        `INSERT INTO invites (id, secret_hash, role, created, expires, uses_left, revoked) VALUES (?, ?, ?, ?, ?, ?, 0)`,
        inv.id,
        blob2(hash),
        role,
        now,
        expires,
        inv.uses_left
      );
      await this.announce(c, "invites", [c.id]);
      return json(201, inviteOut(inv));
    });
  }
  async revokeInvite(c, inviteID) {
    if (!INVITE_ID_RE.test(inviteID)) throw new HttpError(404, "invite_not_found");
    return this.locked(async () => {
      this.access(c.id, false, [ROLE_OWNER]);
      if (this.sql.exec(`SELECT 1 FROM invites WHERE id = ?`, inviteID).toArray().length === 0) {
        throw new HttpError(404, "invite_not_found");
      }
      this.sql.exec(`UPDATE invites SET revoked = 1 WHERE id = ?`, inviteID);
      await this.announce(c, "invites", [c.id]);
      return noContent();
    });
  }
  // checkInvite verifies a link invite's secret (constant time) and that it
  // may still be used now.
  checkInvite(inviteID, hash, now) {
    const r = this.sql.exec(`SELECT * FROM invites WHERE id = ?`, inviteID).toArray()[0];
    if (!r) {
      equalBytes(hash, new Uint8Array(32));
      throw new HttpError(403, "invite_invalid");
    }
    const inv = inviteRow(r);
    if (!equalBytes(hash, inv.secret_hash)) throw new HttpError(403, "invite_invalid");
    if (inv.revoked) throw new HttpError(403, "invite_revoked");
    if (inv.expires !== 0 && now >= inv.expires) throw new HttpError(403, "invite_expired");
    if (inv.uses_left !== null && inv.uses_left <= 0) throw new HttpError(403, "invite_used_up");
    return inv;
  }
  async join(c, request) {
    const body = await readJSONBody(request, MAX_BODY);
    const inviteID = fStr(body, "invite_id");
    const secretText = fStr(body, "secret");
    const pub = fBytes(body, "identity_pub");
    const profile = fBytes(body, "profile");
    if (pub?.length !== IDENTITY_KEY_BYTES) throw new Bad("identity_pub must be 32 bytes");
    if ((profile?.length ?? 0) > MAX_PROFILE_BYTES) throw new Bad("profile must be at most 4096 bytes");
    const secret = decodeRawURL(secretText, SECRET_BYTES);
    if (inviteID !== "" && (!INVITE_ID_RE.test(inviteID) || !secret)) throw new HttpError(403, "invite_invalid");
    const hash = secret ? await sha256(secret) : new Uint8Array(32);
    return this.locked(async () => {
      const now = Date.now();
      if (!c.listed && !this.member(c.id)) {
        if (inviteID === "") throw new HttpError(403, "chain_not_allowed");
        try {
          this.checkInvite(inviteID, hash, now);
        } catch (e) {
          if (e instanceof HttpError && e.code === "invite_invalid") throw new HttpError(403, "chain_not_allowed");
          throw e;
        }
      }
      if (!this.roomRow()) throw new HttpError(403, "invite_invalid");
      const m = this.member(c.id);
      if (m && m.removed === 0 && m.joined !== 0) return this.view(c, 200);
      if (m && m.removed === 0) {
        if (m.identity_pub && !equalBytes(m.identity_pub, pub)) throw new HttpError(409, "identity_mismatch");
        this.sql.exec(
          `UPDATE members SET joined = ?, identity_pub = ?, profile = COALESCE(?, profile) WHERE client_id = ?`,
          now,
          blob2(pub),
          blob2(profile),
          c.id
        );
      } else {
        if (inviteID === "") throw new HttpError(403, m ? "removed" : "invite_invalid");
        const inv = this.checkInvite(inviteID, hash, now);
        if (m && !(inv.created > m.removed)) throw new HttpError(403, "removed");
        const n = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM members WHERE removed = 0`).one().n);
        if (n >= MAX_ROOM_MEMBERS) throw new HttpError(409, "room_full");
        this.ctx.storage.transactionSync(() => {
          this.sql.exec(
            `INSERT INTO members (client_id, role, identity_pub, profile, invited, joined, removed)
             VALUES (?, ?, ?, ?, ?, ?, 0)
             ON CONFLICT (client_id) DO UPDATE SET role = excluded.role, identity_pub = excluded.identity_pub,
               profile = excluded.profile, invited = excluded.invited, joined = excluded.joined, removed = 0`,
            c.id,
            inv.role,
            blob2(pub),
            blob2(profile),
            now,
            now
          );
          if (inv.uses_left !== null) this.sql.exec(`UPDATE invites SET uses_left = uses_left - 1 WHERE id = ?`, inv.id);
        });
      }
      await this.announce(c, "members", this.currentIDs(), { identity: { chain: c.id, pub } });
      return this.view(c, 200);
    });
  }
  async updateMember(c, request, client) {
    const who = client.toLowerCase();
    if (!CHAIN_ID_RE.test(who)) throw new HttpError(404, "member_not_found");
    const body = await readJSONBody(request, MAX_BODY);
    for (const k of Object.keys(body)) if (k !== "role" && k !== "profile") throw new Bad("unknown field " + k);
    const hasRole = "role" in body;
    const hasProfile = "profile" in body;
    if (!hasRole && !hasProfile) throw new Bad("nothing to change");
    let role = "";
    if (hasRole) {
      if (typeof body.role !== "string" || !validRole(body.role)) throw new Bad("role must be editor or viewer");
      role = body.role;
    }
    let profile = null;
    if (hasProfile) {
      try {
        profile = fBytes(body, "profile");
      } catch {
        throw new Bad("profile must be base64 or null");
      }
      if ((profile?.length ?? 0) > MAX_PROFILE_BYTES) throw new Bad("profile must be at most 4096 bytes");
      if (who !== c.id) throw new HttpError(403, "forbidden");
    }
    return this.locked(async () => {
      if (hasRole) {
        this.access(c.id, false, [ROLE_OWNER]);
        const m = this.currentTarget(who);
        if (m.role === ROLE_OWNER) throw new HttpError(403, "forbidden");
        this.sql.exec(`UPDATE members SET role = ? WHERE client_id = ?`, role, m.client_id);
      }
      if (hasProfile) {
        const { me } = this.access(c.id, true);
        this.sql.exec(`UPDATE members SET profile = ? WHERE client_id = ?`, blob2(profile), me.client_id);
      }
      await this.announce(c, "members", this.currentIDs());
      return noContent();
    });
  }
  async removeMember(c, client) {
    const who = client.toLowerCase();
    if (!CHAIN_ID_RE.test(who)) throw new HttpError(404, "member_not_found");
    return this.locked(async () => {
      const self = who === c.id;
      const { me } = this.access(c.id, self);
      if (!self && me.role !== ROLE_OWNER) throw new HttpError(403, "forbidden");
      const m = this.currentTarget(who);
      if (m.role === ROLE_OWNER) throw new HttpError(409, "owner_must_transfer");
      this.ctx.storage.transactionSync(() => {
        this.sql.exec(`UPDATE members SET removed = ? WHERE client_id = ?`, Date.now(), m.client_id);
        this.sql.exec(`DELETE FROM envelopes WHERE client_id = ?`, m.client_id);
      });
      this.wake(m.client_id);
      await this.announce(c, "members", [...this.currentIDs(), m.client_id]);
      return noContent();
    });
  }
  async transfer(c, request) {
    const body = await readJSONBody(request, MAX_BODY);
    const id = fStr(body, "client_id").toLowerCase();
    if (!CHAIN_ID_RE.test(id)) throw new Bad("client_id must be 64 hex digits");
    return this.locked(async () => {
      const { room: room2, me } = this.access(c.id, false, [ROLE_OWNER]);
      const m = this.currentTarget(id);
      if (m.joined === 0) throw new HttpError(403, "not_joined");
      if (m.client_id === me.client_id) return noContent();
      await this.moveBytes(room2, m.client_id);
      this.ctx.storage.transactionSync(() => {
        this.sql.exec(`UPDATE room SET owner = ?`, m.client_id);
        this.sql.exec(`UPDATE members SET role = ? WHERE client_id = ?`, ROLE_EDITOR, me.client_id);
        this.sql.exec(`UPDATE members SET role = ? WHERE client_id = ?`, ROLE_OWNER, m.client_id);
      });
      if (room2.bytes !== 0) await chainStub2(this.env, room2.owner).addRoomBytes(-room2.bytes);
      await this.announce(c, "members", this.currentIDs());
      return noContent();
    });
  }
  // moveBytes counts the room's bytes against chain next (quota_exceeded if
  // they don't fit); the caller gives them back to the old owner after.
  async moveBytes(room2, next) {
    if (room2.bytes > 0 && !await chainStub2(this.env, next).addRoomBytes(room2.bytes)) throw quotaExceeded();
  }
  // successor moves the caller's membership (its role, the owner's room and
  // quota included) to the chain whose token was in X-Compass-Linked-Token
  // (the Worker checked it: X-Linked-Chain, empty if it was bad).
  async successor(c, request) {
    const next = (request.headers.get("X-Linked-Chain") ?? "").toLowerCase();
    if (!next) throw new HttpError(400, "invalid_linked_token");
    if (next === c.id) throw new Bad("the successor is the caller's own chain");
    const body = await readJSONBody(request, MAX_BODY);
    const pub = fBytes(body, "identity_pub");
    const envs = fObjects(body, "envelopes").map((e) => ({ epoch: fInt(e, "epoch"), sealed: fBytes(e, "sealed") }));
    if (pub?.length !== IDENTITY_KEY_BYTES) throw new Bad("identity_pub must be 32 bytes");
    if (envs.length === 0 || envs.length > MAX_ENVELOPES) throw new Bad("envelopes must hold 1 to 1000 envelopes");
    const seen = /* @__PURE__ */ new Set();
    for (const e of envs) {
      if (seen.has(e.epoch)) throw new Bad("two envelopes for one epoch");
      if (e.sealed?.length !== ENVELOPE_BYTES) throw new Bad("sealed must be 80 bytes");
      seen.add(e.epoch);
    }
    if ((await chainStub2(this.env, next).status()).disabled) throw new HttpError(409, "successor_disabled");
    return this.locked(async () => {
      const { room: room2, me } = this.access(c.id, true);
      const there = this.member(next);
      if (there && there.removed === 0) throw new HttpError(409, "already_member");
      let current = false;
      for (const e of envs) {
        if (e.epoch < 1 || e.epoch > room2.epoch) throw new HttpError(409, "stale_epoch");
        current ||= e.epoch === room2.epoch;
      }
      if (!current) throw new HttpError(409, "stale_epoch");
      const owner = me.role === ROLE_OWNER;
      if (owner) await this.moveBytes(room2, next);
      const now = Date.now();
      this.ctx.storage.transactionSync(() => {
        if (owner) this.sql.exec(`UPDATE room SET owner = ?`, next);
        this.sql.exec(
          `INSERT INTO members (client_id, role, identity_pub, profile, invited, joined, removed)
           VALUES (?, ?, ?, ?, ?, ?, 0)
           ON CONFLICT (client_id) DO UPDATE SET role = excluded.role, identity_pub = excluded.identity_pub,
             profile = excluded.profile, invited = excluded.invited, joined = excluded.joined, removed = 0`,
          next,
          me.role,
          blob2(pub),
          blob2(me.profile),
          me.invited,
          me.joined === 0 ? 0 : now
        );
        this.sql.exec(`DELETE FROM envelopes WHERE client_id IN (?, ?)`, me.client_id, next);
        for (const e of envs) this.putEnvelope(next, e.epoch, e.sealed);
        this.sql.exec(
          `UPDATE members SET removed = ?, role = CASE WHEN role = ? THEN ? ELSE role END WHERE client_id = ?`,
          now,
          ROLE_OWNER,
          ROLE_EDITOR,
          me.client_id
        );
      });
      if (owner && room2.bytes !== 0) await chainStub2(this.env, room2.owner).addRoomBytes(-room2.bytes);
      this.wake(me.client_id);
      await this.announce(c, "members", [...this.currentIDs(), me.client_id], { identity: { chain: next, pub } });
      return noContent();
    });
  }
  async rotate(c, request) {
    const body = await readJSONBody(request, MAX_BODY);
    const epoch = fInt(body, "epoch");
    const envs = fObjects(body, "envelopes").map((e) => ({ id: fStr(e, "client_id"), sealed: fBytes(e, "sealed") }));
    if (epoch < 2) throw new Bad("epoch must be 2 or more");
    const byID = /* @__PURE__ */ new Map();
    for (const e of envs) {
      const id = e.id.toLowerCase();
      if (!CHAIN_ID_RE.test(id)) throw new Bad("client_id must be 64 hex digits");
      if (byID.has(id)) throw new Bad("two envelopes for " + id);
      if (e.sealed?.length !== ENVELOPE_BYTES) throw new Bad("sealed must be 80 bytes");
      byID.set(id, e.sealed);
    }
    return this.locked(async () => {
      const { room: room2 } = this.access(c.id, false, [ROLE_OWNER]);
      if (epoch !== room2.epoch + 1) throw new HttpError(409, "stale_epoch");
      const members = this.currentIDs();
      const missing = members.filter((id) => !byID.has(id));
      const unknown = [...byID.keys()].filter((id) => !members.includes(id)).sort();
      if (missing.length > 0 || unknown.length > 0) {
        throw new HttpError(409, "envelopes_mismatch", { missing, unknown });
      }
      this.ctx.storage.transactionSync(() => {
        for (const id of members) this.putEnvelope(id, epoch, byID.get(id));
        this.sql.exec(`UPDATE room SET epoch = ?`, epoch);
      });
      await this.announce(c, "keys", members);
      return noContent();
    });
  }
  async addEnvelopes(c, request) {
    const body = await readJSONBody(request, MAX_BODY);
    const envs = fObjects(body, "envelopes").map((e) => ({
      id: fStr(e, "client_id").toLowerCase(),
      epoch: fInt(e, "epoch"),
      sealed: fBytes(e, "sealed")
    }));
    if (envs.length === 0 || envs.length > MAX_ENVELOPES) throw new Bad("envelopes must hold 1 to 1000 envelopes");
    for (const e of envs) {
      if (!CHAIN_ID_RE.test(e.id)) throw new Bad("client_id must be 64 hex digits");
      if (e.sealed?.length !== ENVELOPE_BYTES) throw new Bad("sealed must be 80 bytes");
    }
    return this.locked(async () => {
      const { room: room2 } = this.access(c.id, false, [ROLE_OWNER]);
      for (const e of envs) {
        if (e.epoch < 1 || e.epoch > room2.epoch) throw new HttpError(409, "stale_epoch");
        this.currentTarget(e.id);
      }
      this.ctx.storage.transactionSync(() => {
        for (const e of envs) this.putEnvelope(e.id, e.epoch, e.sealed);
      });
      await this.announce(c, "keys", [...new Set(envs.map((e) => e.id))].sort());
      return noContent();
    });
  }
  // --- Calls from other objects and the Worker -------------------------------
  // identityChanged records chain's new identity key in its membership, if
  // it's a current member with another key, and tells the members (PUT
  // /v2/compass/identity). device is the calling device.
  async identityChanged(roomID, chain, pub, device) {
    const key = decodeStd(pub);
    if (!key) return;
    await this.locked(async () => {
      const m = this.member(chain);
      if (!m || m.removed !== 0 || m.identity_pub && equalBytes(m.identity_pub, key)) return;
      this.sql.exec(`UPDATE members SET identity_pub = ? WHERE client_id = ?`, blob2(key), m.client_id);
      await this.announce({ id: m.client_id, listed: true, device, room: roomID }, "members", this.currentIDs());
    });
  }
  // chainGone is a chain's data being cleared (reset, idle expiry): a room
  // it owns is deleted, its membership of another goes with its envelopes.
  // Nothing is announced (sync.md, "Going away"); the other members' indexes
  // drop a deleted room.
  async chainGone(roomID, chainID) {
    const chain = chainID.toLowerCase();
    await this.locked(async () => {
      const room2 = this.roomRow();
      if (!room2) return;
      if (room2.owner === chain) {
        const others = this.currentIDs().filter((id) => id !== chain);
        this.wipe();
        this.wake();
        await this.announce({ id: chain, listed: true, device: "", room: roomID }, "deleted", others, {
          seq: room2.seq,
          silent: true
        });
        return;
      }
      this.ctx.storage.transactionSync(() => {
        this.sql.exec(`DELETE FROM members WHERE client_id = ?`, chain);
        this.sql.exec(`DELETE FROM envelopes WHERE client_id = ?`, chain);
      });
      this.wake(chain);
    });
  }
};
function inviteRow(r) {
  return {
    id: r.id,
    secret_hash: bytesOf2(r.secret_hash) ?? new Uint8Array(0),
    role: r.role,
    created: Number(r.created),
    expires: Number(r.expires),
    uses_left: r.uses_left === null ? null : Number(r.uses_left),
    revoked: Number(r.revoked) !== 0
  };
}
__name(inviteRow, "inviteRow");
function inviteOut(inv) {
  return { id: inv.id, role: inv.role, created: inv.created, expires: inv.expires, uses_left: inv.uses_left, revoked: inv.revoked };
}
__name(inviteOut, "inviteOut");

// src/index.ts
var LINKED_TOKEN_HEADER2 = "X-Compass-Linked-Token";
var KEEP_ALIVE_MS = 25e3;
var ACCESS_TTL_MS = 6e4;
var VERSION = `cloudflare-${SERVER_VERSION}`;
var accessCache = /* @__PURE__ */ new Map();
function access(env) {
  return env.ACCESS.get(env.ACCESS.idFromName("access"));
}
__name(access, "access");
async function accessState(env, chain) {
  const key = chain.toLowerCase();
  const now = Date.now();
  const hit = accessCache.get(key);
  if (hit && hit.until > now) return hit.state;
  const state = await access(env).state(key);
  if (accessCache.size > 1e4) accessCache.clear();
  accessCache.set(key, { state, until: now + (state.allowed ? ACCESS_TTL_MS : 1e4) });
  return state;
}
__name(accessState, "accessState");
async function authenticate(env, header) {
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
__name(authenticate, "authenticate");
async function authorize(env, request) {
  const chain = await authenticate(env, request.headers.get("Authorization"));
  if (!chain) return "";
  return (await accessState(env, chain)).allowed ? chain : "";
}
__name(authorize, "authorize");
function chainStub3(env, chain) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}
__name(chainStub3, "chainStub");
async function toChain(env, chain, path, request, init = {}) {
  const url = new URL(request.url);
  const headers = new Headers(init.headers);
  headers.set("X-Chain-ID", chain);
  headers.set("X-Compass-Push", (await accessState(env, chain)).push ? "on" : "off");
  const enc = request.headers.get("Content-Encoding");
  if (enc) headers.set("Content-Encoding", enc);
  return chainStub3(env, chain).fetch(
    new Request(`https://chain${path}${url.search}`, {
      method: request.method,
      body: request.method === "GET" || request.method === "HEAD" ? null : request.body,
      ...init,
      headers
    })
  );
}
__name(toChain, "toChain");
var index_default = {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;
    const method = request.method;
    if (path === "/" && method === "GET") return new Response(".", { status: 200 });
    if (path === "/healthz") return text(200, "ok");
    if (path === "/health-check") return json(200, { status: "ok", version: VERSION, commit: VERSION });
    if (path === "/invite" && method === "GET") return invitePage();
    if (!path.startsWith("/v2/")) return text(404, "404 page not found");
    const rest = path.slice(3);
    if (rest === "/command/") {
      if (method !== "POST") return text(405, "Method Not Allowed");
      const chain = await authorize(env, request);
      if (!chain) return text(401, "Unauthorized");
      const rsp = await toChain(env, chain, "/command/", request);
      forgetGate(chain);
      return rsp;
    }
    if (rest === "/compass/info" && method === "GET") {
      return json(200, { login: null, server: { kind: "cloudflare", version: SERVER_VERSION } });
    }
    const r = await rooms(env, request, rest, {
      authenticate,
      listed: /* @__PURE__ */ __name(async (e, chain) => (await accessState(e, chain)).allowed, "listed")
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
      const guid = rest.slice("/compass/devices".length);
      const rsp = await toChain(env, chain, "/devices" + guid, request);
      forgetGate(chain);
      return rsp;
    }
    if (rest === "/compass/push") {
      if (method !== "POST" && method !== "DELETE") {
        return text(405, "POST or DELETE");
      }
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      return toChain(env, chain, "/push", request, {
        headers: { "X-Host": (env.COMPASS_SYNC_PUBLIC_HOST || url.host).toLowerCase() }
      });
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
          signal: request.signal
        })
      );
    }
    if (rest === "/compass/account/link" && method === "POST") {
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      const raw = (request.headers.get(LINKED_TOKEN_HEADER2) ?? "").trim();
      const other = raw ? await authenticate(env, raw.startsWith("Bearer ") ? raw : "Bearer " + raw) : "";
      if (!other) return fail(400, "invalid " + LINKED_TOKEN_HEADER2);
      accessCache.delete(other.toLowerCase());
      return access(env).link(chain, other);
    }
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
      const relay = loadConfig(env).pushRelay;
      if (rest === "/compass/access" && method === "GET") return access(env).members(chain, relay);
      if (rest === "/compass/access/settings" && method === "PUT") {
        const rsp = await access(env).setSettings(chain, await readJSON(request) ?? {}, relay);
        if (rsp.ok) accessCache.clear();
        return rsp;
      }
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
  }
};
async function readJSON(request) {
  try {
    const v = await request.json();
    return v && typeof v === "object" ? v : null;
  } catch {
    return null;
  }
}
__name(readJSON, "readJSON");
async function events(env, chain, request, ctx) {
  const rsp = await toChain(env, chain, "/events", request, {
    method: "GET",
    headers: { Upgrade: "websocket" },
    body: null
  });
  const ws = rsp.webSocket;
  if (!ws) return rsp;
  ws.accept();
  const { readable, writable } = new TransformStream();
  const writer = writable.getWriter();
  const enc = new TextEncoder();
  let done = false;
  let resolveDone = /* @__PURE__ */ __name(() => {
  }, "resolveDone");
  const finished = new Promise((r) => resolveDone = r);
  const close = /* @__PURE__ */ __name(() => {
    if (done) return;
    done = true;
    clearInterval(timer);
    try {
      ws.close(1e3, "stream closed");
    } catch {
    }
    writer.close().catch(() => {
    });
    resolveDone();
  }, "close");
  const send = /* @__PURE__ */ __name((s) => {
    if (!done) writer.write(enc.encode(s)).catch(close);
  }, "send");
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
      "X-Accel-Buffering": "no"
    }
  });
}
__name(events, "events");
function invitePage() {
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
<script>document.getElementById("link").textContent = location.href;<\/script>
</html>`;
  return new Response(html, {
    status: 200,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "no-store",
      "Referrer-Policy": "no-referrer",
      "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'"
    }
  });
}
__name(invitePage, "invitePage");
export {
  Access,
  Chain,
  Pairing,
  Room,
  index_default as default
};
