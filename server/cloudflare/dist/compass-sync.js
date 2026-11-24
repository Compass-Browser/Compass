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
  const b64 = s.replace(/-/g, "+").replace(/_/g, "/");
  try {
    return atob(b64);
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
    )
  };
}
__name(loadConfig, "loadConfig");
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
    const chain = chainID.toLowerCase();
    const own = this.listed(chain);
    if (own.allowed) return own;
    const root = this.root(chain);
    if (!root) return { allowed: false, owner: false };
    const blocked = idList(this.env.BLOCKED_CLIENT_IDS);
    if (blocked.includes(root)) return { allowed: false, owner: false };
    return this.listed(root);
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
  members(byID) {
    if (!this.state(byID).owner) return fail(403, "owner_only");
    const now = Date.now();
    const members = this.sql.exec(`SELECT chain, added_at, name FROM members ORDER BY added_at`).toArray().map((r) => ({
      chain: r.chain,
      name: r.name,
      added_at: Math.floor(Number(r.added_at) / 1e3),
      linked: Number(this.sql.exec(`SELECT COUNT(*) AS n FROM links WHERE root = ?`, r.chain).one().n)
    }));
    const invites = this.sql.exec(`SELECT code, name, expires_at FROM invites WHERE used_by IS NULL AND expires_at >= ? ORDER BY created_at`, now).toArray().map((r) => ({ code: r.code, name: r.name, expires_in: Math.floor((Number(r.expires_at) - now) / 1e3) }));
    return json(200, { owners: this.owners(), members, invites });
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
    const path = url.pathname;
    if (path === "/command/" && request.method === "POST") return this.command(request, chainID);
    if (path === "/events" && request.method === "GET") return this.events(request, client);
    if (path === "/quota" && request.method === "GET") return this.quota();
    if (path.startsWith("/devices") && request.method === "DELETE") return this.removeDevice(url, client);
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
          this.disable();
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
  notifyCommit(msg, rsp, _isNew) {
    const entries = msg.commit?.entries ?? [];
    const results = rsp.commit?.entryResponse ?? [];
    const types = /* @__PURE__ */ new Set();
    entries.forEach((e, i) => {
      if (results[i]?.responseType !== ResponseType.SUCCESS || !e.specifics) return;
      try {
        types.add(dataTypeOfSpecifics(e.specifics));
      } catch {
      }
    });
    this.notify(msg.commit?.cacheGuid ?? "", [...types].sort((a, b) => a - b));
  }
  async webSocketMessage() {
  }
  async webSocketClose(ws, code) {
    try {
      ws.close(code === 1005 ? 1e3 : code, "closed");
    } catch {
    }
  }
  // --- Maintenance -----------------------------------------------------------
  // disable clears the chain's data and refuses it from now on ("reset
  // sync", and idle chains): ClearServerData.
  disable() {
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`DELETE FROM entities`);
      this.sql.exec(`DELETE FROM unique_tags`);
      this.sql.exec(`DELETE FROM removed_devices`);
      this.sql.exec(
        `UPDATE chain SET disabled_at = ?, bytes = 0, item_count = 0, history_period1 = 0,
           history_period2 = 0, history_period3 = 0, history_period4 = 0, quota_full = 0`,
        Date.now()
      );
    });
    for (const ws of this.ctx.getWebSockets()) ws.close(1e3, "chain disabled");
  }
  // alarm runs hourly while the chain is in use: expired history and old
  // tombstones go, and an idle chain is cleared and disabled.
  async alarm() {
    if (this.isDisabled()) return;
    const now = Date.now();
    const lastActive = this.sql.exec(`SELECT last_active FROM chain`).one().last_active;
    if (this.cfg.idleChainDays > 0 && lastActive !== null && Number(lastActive) < Math.floor(now / 1e3) - this.cfg.idleChainDays * 86400) {
      this.disable();
      return;
    }
    this.purge(now);
    await this.ctx.storage.setAlarm(now + PURGE_INTERVAL_MS);
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

// src/index.ts
var LINKED_TOKEN_HEADER = "X-Compass-Linked-Token";
var KEEP_ALIVE_MS = 25e3;
var ACCESS_TTL_MS = 6e4;
var VERSION = "cloudflare-1";
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
function chainStub(env, chain) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}
__name(chainStub, "chainStub");
function toChain(env, chain, path, request, init = {}) {
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
      return toChain(env, chain, "/command/", request);
    }
    if (rest === "/compass/info" && method === "GET") return json(200, { login: null });
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
      return toChain(env, chain, "/devices" + guid, request);
    }
    if (rest === "/compass/push") {
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
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
          signal: request.signal
        })
      );
    }
    if (rest === "/compass/account/link" && method === "POST") {
      const chain = await authorize(env, request);
      if (!chain) return fail(401, "invalid sync token");
      const raw = (request.headers.get(LINKED_TOKEN_HEADER) ?? "").trim();
      const other = raw ? await authenticate(env, raw.startsWith("Bearer ") ? raw : "Bearer " + raw) : "";
      if (!other) return fail(400, "invalid " + LINKED_TOKEN_HEADER);
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
  index_default as default
};
