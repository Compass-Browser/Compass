// Rooms in the Worker (docs/design/sync.md, "Rooms"): the checks every
// route starts with, then the room's object (room.ts) or the caller's
// chain's object (chain.ts: its rooms, its identity key).
//
// Checks, in the contract's order: the token (401 invalid sync token), a
// removed device (403 device_removed), a disabled chain (403
// chain_disabled), the private server's gate (403 chain_not_allowed; a
// chain the access list doesn't name may still use the rooms it's in),
// the form, the room's rules. The chain's answer about removed devices and
// being disabled is kept a minute per isolate, so a long-polling client
// doesn't wake its chain's object on every poll.

import { type Env, deviceRemoved, fail, json, text } from "./config";
import {
  b64,
  Bad,
  ENVELOPE_BYTES,
  errorResponse,
  fBytes,
  fStr,
  HttpError,
  IDENTITY_KEY_BYTES,
  KINDS,
  LINKED_TOKEN_HEADER,
  MAX_BODY,
  MAX_PROFILE_BYTES,
  MAX_ROOMS_PER_OWNER,
  MAX_WAIT_SECONDS,
  newID,
  noContent,
  readJSONBody,
  validRoomID,
} from "./roomwire";

const GATE_TTL_MS = 60_000;

export interface RoomDeps {
  // The chain of a valid, unblocked token ("" if none).
  authenticate(env: Env, header: string | null): Promise<string>;
  // Whether the private server lists the chain (or has no list).
  listed(env: Env, chain: string): Promise<boolean>;
}

interface Gate {
  disabled: boolean;
  removed: boolean;
}

// The chain's object's answers, per chain and device.
const gateCache = new Map<string, Map<string, { gate: Gate; until: number }>>();

// forgetGate drops what's kept of a chain's answers (after a sync request,
// which may have reset the chain, or a device removal).
export function forgetGate(chain: string): void {
  gateCache.delete(chain.toLowerCase());
}

function chainStub(env: Env, chain: string) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}

function roomStub(env: Env, room: string) {
  return env.ROOM.get(env.ROOM.idFromName(room));
}

async function gate(env: Env, chain: string, device: string): Promise<Gate> {
  const key = chain.toLowerCase();
  const now = Date.now();
  const hit = gateCache.get(key)?.get(device);
  if (hit && hit.until > now) return hit.gate;
  const g = await chainStub(env, chain).roomGate(chain, device);
  if (gateCache.size > 10000) gateCache.clear();
  let byDevice = gateCache.get(key);
  if (!byDevice) gateCache.set(key, (byDevice = new Map()));
  if (byDevice.size > 100) byDevice.clear();
  byDevice.set(device, { gate: g, until: now + GATE_TTL_MS });
  return g;
}

interface Caller {
  id: string; // Lower-case.
  listed: boolean;
  device: string;
}

// Routes under /v2/compass/rooms/{room}, by the path after the room and
// the methods each takes.
const ROOM_ROUTES: [RegExp, string[]][] = [
  [/^$/, ["GET", "DELETE"]],
  [/^\/changes$/, ["GET"]],
  [/^\/records$/, ["POST"]],
  [/^\/invites$/, ["POST"]],
  [/^\/invites\/[^/]+$/, ["DELETE"]],
  [/^\/join$/, ["POST"]],
  [/^\/members$/, ["POST"]],
  [/^\/members\/[^/]+$/, ["PATCH", "DELETE"]],
  [/^\/(owner|successor|epochs|envelopes)$/, ["POST"]],
];

// rooms answers the rooms routes, or returns null for any other path
// (rest: the path after /v2).
export async function rooms(
  env: Env,
  request: Request,
  rest: string,
  deps: RoomDeps,
): Promise<Response | null> {
  const method = request.method;
  let kind: "list" | "room" | "identity" | "directory";
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
  const c: Caller = { id: chain.toLowerCase(), listed: await deps.listed(env, chain), device };

  try {
    switch (kind) {
      case "list":
        return method === "GET" ? await listRooms(env, c) : await createRoom(env, request, c);
      case "identity":
        return method === "GET" ? await getIdentity(env, c) : await putIdentity(env, request, c);
      case "directory":
        // The directory needs company sign-in, which this server hasn't.
        if (!c.listed) return fail(403, "chain_not_allowed");
        return fail(404, "directory_disabled");
      case "room":
        return await room(env, request, url, c, roomID, sub, deps);
    }
  } catch (e) {
    return errorResponse(e);
  }
}

async function listRooms(env: Env, c: Caller): Promise<Response> {
  const out = await chainStub(env, c.id).listRooms(c.listed);
  if (!out) return fail(403, "chain_not_allowed");
  return json(200, out);
}

async function createRoom(env: Env, request: Request, c: Caller): Promise<Response> {
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
  if ((await chainStub(env, c.id).ownedRoomCount()) >= MAX_ROOMS_PER_OWNER) {
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
        profile: profile && profile.length > 0 ? b64(profile) : null,
      }),
    }),
  );
}

async function hasRoomsOrListed(env: Env, c: Caller): Promise<void> {
  if (!c.listed && !(await chainStub(env, c.id).hasRooms())) throw new HttpError(403, "chain_not_allowed");
}

async function getIdentity(env: Env, c: Caller): Promise<Response> {
  await hasRoomsOrListed(env, c);
  return json(200, { identity_pub: await chainStub(env, c.id).identityKey() });
}

// putIdentity records the caller's identity key, and in its current
// memberships (whose members get a "members" event).
async function putIdentity(env: Env, request: Request, c: Caller): Promise<Response> {
  await hasRoomsOrListed(env, c);
  const body = await readJSONBody(request, MAX_BODY);
  const pub = fBytes(body, "identity_pub");
  if (pub?.length !== IDENTITY_KEY_BYTES) throw new Bad("identity_pub must be 32 bytes");
  const key = b64(pub);
  const rooms = await chainStub(env, c.id).setIdentityKey(key);
  await Promise.all(
    rooms.map((r) =>
      roomStub(env, r)
        .identityChanged(r, c.id, key, c.device)
        .catch((e: unknown) => console.error("rooms: updating an identity key failed", e)),
    ),
  );
  return noContent();
}

function roomHeaders(c: Caller, roomID: string, extra: Record<string, string> = {}): Headers {
  return new Headers({
    "X-Chain-ID": c.id,
    "X-Listed": c.listed ? "1" : "0",
    "X-Client-ID": c.device,
    "X-Room-ID": roomID,
    ...extra,
  });
}

async function room(
  env: Env,
  request: Request,
  url: URL,
  c: Caller,
  roomID: string,
  sub: string,
  deps: RoomDeps,
): Promise<Response> {
  if (!validRoomID(roomID)) {
    // As the Go server, a join reads its body before the room ID.
    if (sub === "/join") await readJSONBody(request, MAX_BODY);
    return fail(404, "not_found");
  }
  const extra: Record<string, string> = {};
  let path = sub || "/";
  const linked = (request.headers.get(LINKED_TOKEN_HEADER) ?? "").trim();
  if (sub === "/successor" || (sub === "/owner" && linked !== "")) {
    // The successor form: the linked token proves control of the new chain
    // (signature, time, BLOCKED_CLIENT_IDS; not the access list).
    path = "/successor";
    const next = linked ? await deps.authenticate(env, linked.startsWith("Bearer ") ? linked : "Bearer " + linked) : "";
    extra["X-Linked-Chain"] = next.toLowerCase();
  }
  const stub = roomStub(env, roomID);
  const forward = (headers: Headers) =>
    stub.fetch(
      new Request(`https://room${path}${url.search}`, {
        method: request.method,
        headers,
        body: request.method === "GET" || request.method === "DELETE" ? null : request.body,
      }),
    );
  if (sub === "/changes") {
    const w = url.searchParams.get("wait") ?? "";
    const wait = /^\d+$/.test(w) ? Number(w) : 0;
    if (wait > 0 && wait <= MAX_WAIT_SECONDS) return longPoll(request, roomHeaders(c, roomID), wait, forward);
  }
  return forward(roomHeaders(c, roomID, extra));
}

// longPoll serves GET .../changes?wait=N without keeping the room's object
// awake: the object takes a hibernating WebSocket, answers {"seq","epoch"}
// on it, and sends "wake" when a record is written, the caller is removed
// or the room deleted; this Worker waits up to N seconds for that, then
// asks the object again (or, with nothing new, answers an empty page from
// what the object said). A request the object answers at once (records
// already newer than since, an error) comes back as is.
async function longPoll(
  request: Request,
  headers: Headers,
  wait: number,
  forward: (h: Headers) => Promise<Response>,
): Promise<Response> {
  const up = new Headers(headers);
  up.set("Upgrade", "websocket");
  const rsp = await forward(up);
  const ws = rsp.webSocket;
  if (!ws) return rsp;
  ws.accept();
  return new Promise<Response>((resolve) => {
    let hello: { seq: number; epoch: number } | null = null;
    let done = false;
    const finish = (timedOut: boolean) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      try {
        ws.close(1000, "done");
      } catch {
        // Already closed.
      }
      if (timedOut && hello) {
        resolve(json(200, { seq: hello.seq, epoch: hello.epoch, records: [], more: false }));
      } else {
        resolve(forward(headers));
      }
    };
    const timer = setTimeout(() => finish(true), wait * 1000);
    ws.addEventListener("message", (ev) => {
      if (typeof ev.data !== "string") return;
      if (ev.data === "wake") return finish(false);
      try {
        hello = JSON.parse(ev.data) as { seq: number; epoch: number };
      } catch {
        finish(false);
      }
    });
    ws.addEventListener("close", () => finish(false));
    ws.addEventListener("error", () => finish(false));
    request.signal?.addEventListener?.("abort", () => finish(true));
  });
}
