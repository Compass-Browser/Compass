// Rooms (docs/design/sync.md, "Rooms"): what the Worker (rooms.ts), the
// room's object (room.ts) and the chain's object (chain.ts) share: the
// contract's limits, its encodings (base64 as Go's encoding/json writes
// []byte, unpadded base64url for room and invite IDs and secrets), JSON
// bodies read as the Go server's decoder does, and the message a room's
// object sends a member chain's object about a change.

import { json } from "./config";

// Limits of the API (server/rooms/rooms.go, server/datastore/rooms.go).
export const MAX_WAIT_SECONDS = 30;
export const MAX_RECORDS_PER_WRITE = 500;
export const MAX_CHANGES_LIMIT = 1000;
export const MAX_WRITE_BYTES = 24 << 20; // A records request's body.
export const MAX_BATCH_CIPHERTEXT = 16 << 20; // Ciphertext in one records request.
export const MAX_BODY = 1 << 20; // Any other request's body.
export const ENVELOPE_BYTES = 80;
export const RECORD_OVERHEAD = 28;
export const MAX_PROFILE_BYTES = 4096;
export const MAX_INVITE_USES = 1000;
export const IDENTITY_KEY_BYTES = 32;
export const SECRET_BYTES = 32;
export const ROOM_ID_BYTES = 16;
export const MAX_ROOM_MEMBERS = 50;
export const MAX_ROOM_RECORD_BYTES = 1 << 20;
export const MAX_ROOM_INVITES = 100;
export const MAX_ROOMS_PER_OWNER = 1000;
export const MAX_CHANGES_BYTES = 8 << 20;
export const MAX_ENVELOPES = 1000;

export const RECORD_ID_RE = /^[A-Za-z0-9._:-]{1,128}$/;
const ROOM_ID_RE = /^[A-Za-z0-9_-]{22}$/;
export const INVITE_ID_RE = /^[A-Za-z0-9_-]{1,64}$/;
export const SECRET_HASH_RE = /^[0-9a-f]{64}$/;
export const CHAIN_ID_RE = /^[0-9a-f]{64}$/;

export const ROLE_OWNER = "owner";
export const ROLE_EDITOR = "editor";
export const ROLE_VIEWER = "viewer";
export const KINDS = ["space", "folder", "easel"];

export const LINKED_TOKEN_HEADER = "X-Compass-Linked-Token";

// Membership is what a member chain's object keeps of one of its rooms
// (its index, for GET /v2/compass/rooms); null when it isn't a current
// member any more.
export interface Membership {
  kind: string;
  owner: string;
  role: string;
  pending: boolean;
  epoch: number;
  seq: number;
}

// RoomEvent is a room's object telling a member chain's object of a change
// (Chain.roomEvent).
export interface RoomEvent {
  chain: string; // The member chain, lower-case.
  room: string;
  seq: number;
  changes: string[]; // Sorted.
  skipClient: string; // The device (cache GUID) whose stream isn't told.
  membership: Membership | null;
  silent?: boolean; // Only the index changes: no "room" event.
  identity?: string; // The chain's identity key to record (base64).
}

// --- Encodings ---------------------------------------------------------------

export function b64(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(s);
}

function fromBinary(s: string): Uint8Array {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

// decodeStd decodes base64 as Go's encoding/json does for []byte (the
// standard alphabet with padding; line breaks ignored), or returns null.
export function decodeStd(s: string): Uint8Array | null {
  const t = s.replace(/[\r\n]/g, "");
  if (t.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(t)) return null;
  try {
    return fromBinary(atob(t));
  } catch {
    return null;
  }
}

export function rawURL(bytes: Uint8Array): string {
  return b64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

// decodeRawURL decodes canonical unpadded base64url of exactly n bytes
// (Go's base64.RawURLEncoding.Strict()), or returns null.
export function decodeRawURL(s: string, n: number): Uint8Array | null {
  if (!/^[A-Za-z0-9_-]*$/.test(s)) return null;
  let std = s.replace(/-/g, "+").replace(/_/g, "/");
  while (std.length % 4) std += "=";
  let out: Uint8Array;
  try {
    out = fromBinary(atob(std));
  } catch {
    return null;
  }
  if (out.length !== n || rawURL(out) !== s) return null;
  return out;
}

// validRoomID says whether id is canonical unpadded base64url of 16 bytes.
export function validRoomID(id: string): boolean {
  return ROOM_ID_RE.test(id) && decodeRawURL(id, ROOM_ID_BYTES) !== null;
}

// newID is 16 random bytes as unpadded base64url (room and invite IDs).
export function newID(): string {
  return rawURL(crypto.getRandomValues(new Uint8Array(16)));
}

export async function sha256(b: Uint8Array): Promise<Uint8Array> {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", b));
}

// equalBytes compares in constant time for equal lengths.
export function equalBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a[i] ^ b[i];
  return d === 0;
}

// --- Answers -----------------------------------------------------------------

export class HttpError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    readonly extra: Record<string, unknown> = {},
  ) {
    super(code);
  }
}

// Bad is a malformed body or parameter (400 bad_request with a detail).
export class Bad extends Error {}

export function badRequest(detail: string): Response {
  return json(400, { error: "bad_request", detail });
}

export function noContent(): Response {
  return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
}

export function errorResponse(e: unknown): Response {
  if (e instanceof HttpError) return json(e.status, { error: e.code, ...e.extra });
  if (e instanceof Bad) return badRequest(e.message);
  console.error("rooms: request failed", e);
  return json(500, { error: "unable to complete request" });
}

// --- Bodies ------------------------------------------------------------------

type Obj = Record<string, unknown>;

// readJSONBody reads a body of at most limit bytes holding one JSON object,
// as the Go server's decode(): 413 too_large past the limit, 400 for
// anything but one JSON value (null reads as an empty object).
export async function readJSONBody(request: Request, limit: number): Promise<Obj> {
  const parts: Uint8Array[] = [];
  let size = 0;
  if (request.body) {
    const reader = request.body.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > limit) {
        await reader.cancel().catch(() => {});
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
  let v: unknown;
  try {
    v = JSON.parse(new TextDecoder().decode(buf));
  } catch (e) {
    throw new Bad("invalid JSON: " + (e instanceof Error ? e.message : String(e)));
  }
  if (v === null) return {};
  if (typeof v !== "object" || Array.isArray(v)) throw new Bad("invalid JSON: not an object");
  return v as Obj;
}

// Field readers: absent or null is the zero value (as Go's decoder), a
// value of another type is a 400.

function typeError(k: string, want: string): Bad {
  return new Bad(`invalid JSON: ${k} must be ${want}`);
}

export function fStr(o: Obj, k: string): string {
  const v = o[k];
  if (v === undefined || v === null) return "";
  if (typeof v !== "string") throw typeError(k, "a string");
  return v;
}

export function fOptInt(o: Obj, k: string): number | null {
  const v = o[k];
  if (v === undefined || v === null) return null;
  if (typeof v !== "number" || !Number.isSafeInteger(v)) throw typeError(k, "an integer");
  return v;
}

export function fInt(o: Obj, k: string): number {
  return fOptInt(o, k) ?? 0;
}

export function fBool(o: Obj, k: string): boolean {
  const v = o[k];
  if (v === undefined || v === null) return false;
  if (typeof v !== "boolean") throw typeError(k, "a boolean");
  return v;
}

export function fBytes(o: Obj, k: string): Uint8Array | null {
  const v = o[k];
  if (v === undefined || v === null) return null;
  if (typeof v !== "string") throw typeError(k, "base64");
  const b = decodeStd(v);
  if (!b) throw typeError(k, "base64");
  return b;
}

export function fObjects(o: Obj, k: string): Obj[] {
  const v = o[k];
  if (v === undefined || v === null) return [];
  if (!Array.isArray(v)) throw typeError(k, "an array");
  return v.map((e) => {
    if (e === null) return {};
    if (typeof e !== "object" || Array.isArray(e)) throw typeError(k, "an array of objects");
    return e as Obj;
  });
}
