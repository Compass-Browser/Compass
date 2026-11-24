// One room (docs/design/sync.md, "Rooms"): a Durable Object per room ID
// holding the room's row, members, records, envelopes and invites in its
// own SQLite database. A port of the Go server's rooms
// (server/rooms/rooms.go, server/datastore/rooms.go) with the same routes,
// checks and answers.
//
// The Worker (rooms.ts) has checked the token, removed devices, the
// disabled chain and the room ID's form, and passes the caller in headers:
// X-Chain-ID (lower case), X-Listed ("1": the private server lists it),
// X-Client-ID (the calling device), X-Room-ID, and X-Linked-Chain on the
// successor route. Everything else (the private server's gate for rooms,
// the form of the body, the room's rules) is checked here.
//
// Elsewhere:
// - the room's bytes count against its owner chain's quota in that chain's
//   object (Chain.addRoomBytes);
// - every change is told to each member chain's object (Chain.roomEvent),
//   which sends the "room" event to the chain's streams and keeps its index
//   of rooms for GET /v2/compass/rooms;
// - long polls of GET .../changes?wait= are hibernating WebSockets: this
//   object sends "wake" when a record is written, the watcher is removed or
//   the room is deleted, and costs nothing while it waits.

import { DurableObject } from "cloudflare:workers";
import { type Env, json, text } from "./config";
import {
  b64,
  Bad,
  CHAIN_ID_RE,
  decodeRawURL,
  decodeStd,
  ENVELOPE_BYTES,
  equalBytes,
  errorResponse,
  fBool,
  fBytes,
  fInt,
  fObjects,
  fOptInt,
  fStr,
  HttpError,
  IDENTITY_KEY_BYTES,
  INVITE_ID_RE,
  MAX_BATCH_CIPHERTEXT,
  MAX_BODY,
  MAX_CHANGES_BYTES,
  MAX_CHANGES_LIMIT,
  MAX_ENVELOPES,
  MAX_INVITE_USES,
  MAX_PROFILE_BYTES,
  MAX_RECORDS_PER_WRITE,
  MAX_ROOM_INVITES,
  MAX_ROOM_MEMBERS,
  MAX_ROOM_RECORD_BYTES,
  MAX_WAIT_SECONDS,
  MAX_WRITE_BYTES,
  type Membership,
  newID,
  noContent,
  readJSONBody,
  RECORD_ID_RE,
  RECORD_OVERHEAD,
  ROLE_EDITOR,
  ROLE_OWNER,
  ROLE_VIEWER,
  type RoomEvent,
  SECRET_BYTES,
  SECRET_HASH_RE,
  sha256,
} from "./roomwire";

const MAX_WATCHERS = 1000;

const SCHEMA = `
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

interface RoomRow {
  id: string;
  owner: string;
  kind: string;
  created: number;
  seq: number;
  epoch: number;
  bytes: number;
}

interface MemberRow {
  client_id: string;
  role: string;
  identity_pub: Uint8Array | null;
  profile: Uint8Array | null;
  invited: number;
  joined: number; // 0: pending.
  removed: number; // 0: current.
}

interface InviteRow {
  id: string;
  secret_hash: Uint8Array;
  role: string;
  created: number;
  expires: number;
  uses_left: number | null;
  revoked: boolean;
}

interface RecordOut {
  id: string;
  seq: number;
  epoch: number;
  deleted: boolean;
  author: string;
  ciphertext?: string;
}

// The request's caller, as the Worker passed it.
interface Caller {
  id: string; // Lower-case chain ID.
  listed: boolean;
  device: string;
  room: string;
}

type Row = Record<string, SqlStorageValue>;

function bytesOf(v: unknown): Uint8Array | null {
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (v instanceof Uint8Array) return v;
  return null;
}

// blob binds bytes, storing empty ones as NULL (as the Go store).
function blob(b: Uint8Array | null | undefined): ArrayBuffer | null {
  if (!b || b.length === 0) return null;
  return b.slice().buffer as ArrayBuffer;
}

function b64OrNull(b: Uint8Array | null): string | null {
  return b === null ? null : b64(b);
}

function validRole(role: string): boolean {
  return role === ROLE_EDITOR || role === ROLE_VIEWER;
}

function chainStub(env: Env, chain: string) {
  return env.CHAIN.get(env.CHAIN.idFromName(chain.toLowerCase()));
}

function recordOut(r: Row): RecordOut {
  const out: RecordOut = {
    id: r.record_id as string,
    seq: Number(r.seq),
    epoch: Number(r.epoch),
    deleted: Number(r.deleted) !== 0,
    author: r.author as string,
  };
  const ct = bytesOf(r.ciphertext);
  if (ct && ct.length > 0) out.ciphertext = b64(ct);
  return out;
}

function parseCount(v: string | null, def: number): number | null {
  if (v === null || v === "") return def;
  if (!/^[+-]?\d+$/.test(v)) return null;
  const n = Number(v);
  return Number.isSafeInteger(n) ? n : null;
}

const quotaExceeded = () => new HttpError(507, "quota_exceeded");

export class Room extends DurableObject<Env> {
  private sql: SqlStorage;
  private queue: Promise<unknown> = Promise.resolve();

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(SCHEMA);
  }

  // locked runs the room's changes one at a time: they await other objects
  // (quota, events) between reading and writing.
  private locked<T>(fn: () => Promise<T>): Promise<T> {
    const p = this.queue.then(fn, fn);
    this.queue = p.catch(() => {});
    return p;
  }

  // --- HTTP ------------------------------------------------------------------

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const h = request.headers;
    const c: Caller = {
      id: (h.get("X-Chain-ID") ?? "").toLowerCase(),
      listed: h.get("X-Listed") === "1",
      device: h.get("X-Client-ID") ?? "",
      room: h.get("X-Room-ID") ?? "",
    };
    if (!c.id || !c.room) return text(401, "Unauthorized");
    const path = url.pathname;
    const method = request.method;
    try {
      if (path === "/create" && method === "POST") return await this.create(c, request);
      if (path === "/join" && method === "POST") return await this.join(c, request);
      const handler = this.route(method, path);
      if (!handler) return text(404, "404 page not found");
      // The private server's gate: a chain it doesn't list must be, or have
      // been, a member of this room.
      if (!c.listed && !this.member(c.id)) throw new HttpError(403, "chain_not_allowed");
      return await handler(c, request, url);
    } catch (e) {
      return errorResponse(e);
    }
  }

  private route(
    method: string,
    path: string,
  ): ((c: Caller, request: Request, url: URL) => Promise<Response> | Response) | null {
    const seg = path.split("/").filter(Boolean);
    const [a, b] = seg;
    const is = (m: string) => method === m;
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
        // Email invites need company sign-in, which this server hasn't.
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

  private roomRow(): RoomRow | null {
    const r = this.sql.exec(`SELECT id, owner, kind, created, seq, epoch, bytes FROM room`).toArray()[0];
    if (!r) return null;
    return {
      id: r.id as string,
      owner: r.owner as string,
      kind: r.kind as string,
      created: Number(r.created),
      seq: Number(r.seq),
      epoch: Number(r.epoch),
      bytes: Number(r.bytes),
    };
  }

  private memberOf(r: Row): MemberRow {
    return {
      client_id: r.client_id as string,
      role: r.role as string,
      identity_pub: bytesOf(r.identity_pub),
      profile: bytesOf(r.profile),
      invited: Number(r.invited),
      joined: Number(r.joined),
      removed: Number(r.removed),
    };
  }

  private member(chain: string): MemberRow | null {
    const r = this.sql.exec(`SELECT * FROM members WHERE client_id = ?`, chain.toLowerCase()).toArray()[0];
    return r ? this.memberOf(r) : null;
  }

  // currentMembers are the joined and pending members, by client ID.
  private currentMembers(): MemberRow[] {
    return this.sql
      .exec(`SELECT * FROM members WHERE removed = 0 ORDER BY client_id`)
      .toArray()
      .map((r) => this.memberOf(r));
  }

  private currentIDs(): string[] {
    return this.currentMembers().map((m) => m.client_id);
  }

  // access loads the room and the caller's membership: not_found if either
  // is missing, removed, not_joined if pending and !pendingOK, forbidden if
  // its role isn't in roles (none: any).
  private access(caller: string, pendingOK: boolean, roles: string[] = []): { room: RoomRow; me: MemberRow } {
    const room = this.roomRow();
    if (!room) throw new HttpError(404, "not_found");
    const me = this.member(caller);
    if (!me) throw new HttpError(404, "not_found");
    if (me.removed !== 0) throw new HttpError(403, "removed");
    if (me.joined === 0 && !pendingOK) throw new HttpError(403, "not_joined");
    if (roles.length > 0 && !roles.includes(me.role)) throw new HttpError(403, "forbidden");
    return { room, me };
  }

  // currentTarget is a member operation's target: member_not_found unless
  // it's a current member.
  private currentTarget(chain: string): MemberRow {
    const m = this.member(chain);
    if (!m || m.removed !== 0) throw new HttpError(404, "member_not_found");
    return m;
  }

  private putEnvelope(chain: string, epoch: number, sealed: Uint8Array): void {
    this.sql.exec(
      `INSERT INTO envelopes (client_id, epoch, sealed) VALUES (?, ?, ?)
       ON CONFLICT (client_id, epoch) DO UPDATE SET sealed = excluded.sealed`,
      chain, epoch, blob(sealed),
    );
  }

  private wipe(): void {
    this.ctx.storage.transactionSync(() => {
      for (const t of ["room", "members", "records", "envelopes", "invites"]) this.sql.exec(`DELETE FROM ${t}`);
    });
  }

  // --- Notifications ---------------------------------------------------------

  private membership(chain: string, room: RoomRow | null): Membership | null {
    if (!room) return null;
    const m = this.member(chain);
    if (!m || m.removed !== 0) return null;
    return { kind: room.kind, owner: room.owner, role: m.role, pending: m.joined === 0, epoch: room.epoch, seq: room.seq };
  }

  // announce tells each chain of targets of a change to the room: a "room"
  // event on its streams (but the calling device's) and its membership as
  // it now is, for its index. Failures are logged: a member finds out at
  // its next request.
  private async announce(
    c: Caller,
    change: string,
    targets: string[],
    opts: { seq?: number; silent?: boolean; identity?: { chain: string; pub: Uint8Array } } = {},
  ): Promise<void> {
    const room = this.roomRow();
    const seq = opts.seq ?? room?.seq ?? 0;
    await Promise.all(
      [...new Set(targets)].map(async (t) => {
        const ev: RoomEvent = {
          chain: t,
          room: c.room,
          seq,
          changes: [change],
          skipClient: t === c.id ? c.device : "",
          membership: this.membership(t, room),
        };
        if (opts.silent) ev.silent = true;
        if (opts.identity && opts.identity.chain === t) ev.identity = b64(opts.identity.pub);
        try {
          await chainStub(this.env, t).roomEvent(ev);
        } catch (e) {
          console.error("rooms: telling a member failed", e);
        }
      }),
    );
  }

  // wake ends the long polls of chain (all: every one).
  private wake(chain?: string): void {
    for (const ws of this.ctx.getWebSockets(chain ? "w:" + chain : undefined)) {
      try {
        ws.send("wake");
      } catch {
        // Closing.
      }
    }
  }

  async webSocketMessage(): Promise<void> {
    // Watchers only listen.
  }

  async webSocketClose(ws: WebSocket, code: number): Promise<void> {
    try {
      ws.close(code === 1005 ? 1000 : code, "closed");
    } catch {
      // Already closed.
    }
  }

  // --- Answers ---------------------------------------------------------------

  // view answers the room as c sees it.
  private view(c: Caller, status: number): Response {
    const { room, me } = this.access(c.id, true);
    const members = this.currentMembers().map((m) => ({
      client_id: m.client_id,
      role: m.role,
      identity_pub: b64OrNull(m.identity_pub),
      profile: b64OrNull(m.profile),
      name: "", // No company sign-in on this server.
      email: "",
      invited: m.invited,
      joined: m.joined,
      pending: m.joined === 0,
    }));
    const envelopes = this.sql
      .exec(`SELECT epoch, sealed FROM envelopes WHERE client_id = ? ORDER BY epoch`, me.client_id)
      .toArray()
      .map((r) => ({ epoch: Number(r.epoch), sealed: b64(bytesOf(r.sealed) ?? new Uint8Array(0)) }));
    const out: Record<string, unknown> = {
      id: room.id,
      kind: room.kind,
      owner: room.owner,
      created: room.created,
      epoch: room.epoch,
      seq: room.seq,
      bytes: room.bytes,
      role: me.role,
      pending: me.joined === 0,
      members,
      envelopes,
    };
    if (me.role === ROLE_OWNER) out.invites = this.invites().map(inviteOut);
    return json(status, out);
  }

  private invites(): InviteRow[] {
    return this.sql
      .exec(`SELECT * FROM invites ORDER BY created, id`)
      .toArray()
      .map(inviteRow);
  }

  // --- Routes ----------------------------------------------------------------

  // create makes the room (the Worker checked the body, the caller's right
  // to create and its number of rooms).
  private async create(c: Caller, request: Request): Promise<Response> {
    const body = (await request.json()) as { kind: string; identity_pub: string; envelope: string; profile: string | null };
    const pub = decodeStd(body.identity_pub);
    const envelope = decodeStd(body.envelope);
    const profile = body.profile ? decodeStd(body.profile) : null;
    return this.locked(async () => {
      if (this.roomRow()) throw new HttpError(409, "room_exists");
      const now = Date.now();
      this.ctx.storage.transactionSync(() => {
        this.sql.exec(
          `INSERT INTO room (one, id, owner, kind, created, seq, epoch, bytes) VALUES (1, ?, ?, ?, ?, 0, 1, 0)`,
          c.room, c.id, body.kind, now,
        );
        this.sql.exec(
          `INSERT INTO members (client_id, role, identity_pub, profile, invited, joined, removed)
           VALUES (?, ?, ?, ?, ?, ?, 0)`,
          c.id, ROLE_OWNER, blob(pub), blob(profile), now, now,
        );
        this.putEnvelope(c.id, 1, envelope!);
      });
      await this.announce(c, "members", [c.id], { silent: true, identity: { chain: c.id, pub: pub! } });
      return this.view(c, 201);
    });
  }

  private async deleteRoom(c: Caller): Promise<Response> {
    return this.locked(async () => {
      const { room } = this.access(c.id, false, [ROLE_OWNER]);
      const members = this.currentIDs();
      this.wipe();
      this.wake();
      if (room.bytes !== 0) await chainStub(this.env, room.owner).addRoomBytes(-room.bytes);
      await this.announce(c, "deleted", members, { seq: room.seq });
      return noContent();
    });
  }

  // changes answers GET .../changes. A WebSocket request (the Worker's long
  // poll) with nothing new becomes a watcher: it gets {"seq","epoch"} now
  // and "wake" when it should ask again.
  private changes(c: Caller, request: Request, url: URL): Response {
    const q = url.searchParams;
    const since = parseCount(q.get("since"), 0);
    if (since === null || since < 0) throw new Bad("since must be a sequence number");
    const limit = parseCount(q.get("limit"), MAX_CHANGES_LIMIT);
    if (limit === null || limit < 1 || limit > MAX_CHANGES_LIMIT) throw new Bad("limit must be 1 to 1000");
    const wait = parseCount(q.get("wait"), 0);
    if (wait === null || wait < 0 || wait > MAX_WAIT_SECONDS) throw new Bad("wait must be 0 to 30 seconds");
    const { room } = this.access(c.id, false);
    const records: RecordOut[] = [];
    let total = 0;
    let more = false;
    for (const r of this.sql.exec(
      `SELECT record_id, seq, epoch, deleted, author, ciphertext FROM records WHERE seq > ? ORDER BY seq LIMIT ?`,
      since, limit + 1,
    )) {
      if (records.length === limit || (records.length > 0 && total >= MAX_CHANGES_BYTES)) {
        more = true;
        break;
      }
      total += bytesOf(r.ciphertext)?.length ?? 0;
      records.push(recordOut(r));
    }
    const watch = request.headers.get("Upgrade") === "websocket";
    if (!watch || wait === 0 || records.length > 0 || this.ctx.getWebSockets().length >= MAX_WATCHERS) {
      return json(200, { seq: room.seq, epoch: room.epoch, records, more });
    }
    const pair = new WebSocketPair();
    const [outside, inside] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(inside, ["w:" + c.id]);
    inside.send(JSON.stringify({ seq: room.seq, epoch: room.epoch }));
    return new Response(null, { status: 101, webSocket: outside });
  }

  private async records(c: Caller, request: Request): Promise<Response> {
    const body = await readJSONBody(request, MAX_WRITE_BYTES);
    const raw = fObjects(body, "records").map((r) => ({
      id: fStr(r, "id"),
      epoch: fInt(r, "epoch"),
      deleted: fBool(r, "deleted"),
      ciphertext: fBytes(r, "ciphertext"),
      baseSeq: fOptInt(r, "base_seq"),
    }));
    if (raw.length === 0 || raw.length > MAX_RECORDS_PER_WRITE) throw new Bad("records must hold 1 to 500 records");
    const seen = new Set<string>();
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
      const { room, me } = this.access(c.id, false, [ROLE_OWNER, ROLE_EDITOR]);
      const conflicts: RecordOut[] = [];
      const oldSizes: number[] = [];
      for (const w of raw) {
        if (w.epoch !== room.epoch) throw new HttpError(409, "stale_epoch");
        const old = this.sql
          .exec(`SELECT record_id, seq, epoch, deleted, author, ciphertext, size FROM records WHERE record_id = ?`, w.id)
          .toArray()[0];
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
        const ctLen = w.deleted ? 0 : (w.ciphertext?.length ?? 0);
        delta += ctLen + w.id.length - oldSizes[i];
      });
      const owner = chainStub(this.env, room.owner);
      if (delta > 0 && !(await owner.addRoomBytes(delta))) throw quotaExceeded();
      const out: { id: string; seq: number }[] = [];
      let seq = room.seq;
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
              w.id, seq, w.epoch, w.deleted ? 1 : 0, me.client_id, blob(ct), size,
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

  private async createInvite(c: Caller, request: Request): Promise<Response> {
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
    if (expires < 0 || (expires !== 0 && expires <= now)) throw new Bad("expires must be 0 or in the future");
    if (uses < 0 || uses > MAX_INVITE_USES) throw new Bad("uses must be 0 to 1000");
    const hash = new Uint8Array(secretHash.match(/../g)!.map((x) => parseInt(x, 16)));
    return this.locked(async () => {
      this.access(c.id, false, [ROLE_OWNER]);
      const open = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM invites WHERE revoked = 0`).one().n);
      if (open >= MAX_ROOM_INVITES) throw new HttpError(409, "too_many_invites");
      const inv: InviteRow = {
        id: newID(),
        secret_hash: hash,
        role,
        created: now,
        expires,
        uses_left: uses === 0 ? null : uses,
        revoked: false,
      };
      this.sql.exec(
        `INSERT INTO invites (id, secret_hash, role, created, expires, uses_left, revoked) VALUES (?, ?, ?, ?, ?, ?, 0)`,
        inv.id, blob(hash), role, now, expires, inv.uses_left,
      );
      await this.announce(c, "invites", [c.id]);
      return json(201, inviteOut(inv));
    });
  }

  private async revokeInvite(c: Caller, inviteID: string): Promise<Response> {
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
  private checkInvite(inviteID: string, hash: Uint8Array, now: number): InviteRow {
    const r = this.sql.exec(`SELECT * FROM invites WHERE id = ?`, inviteID).toArray()[0];
    if (!r) {
      // Compare anyway, so a wrong ID and a wrong secret take as long.
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

  private async join(c: Caller, request: Request): Promise<Response> {
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
        // The gate: a pending (or former) member, or a valid invite.
        if (inviteID === "") throw new HttpError(403, "chain_not_allowed");
        try {
          this.checkInvite(inviteID, hash, now);
        } catch (e) {
          if (e instanceof HttpError && e.code === "invite_invalid") throw new HttpError(403, "chain_not_allowed");
          throw e;
        }
      }
      // Without an invite that opens it, a room is never shown to exist.
      if (!this.roomRow()) throw new HttpError(403, "invite_invalid");
      const m = this.member(c.id);
      if (m && m.removed === 0 && m.joined !== 0) return this.view(c, 200);
      if (m && m.removed === 0) {
        // Pending (an email invite).
        if (m.identity_pub && !equalBytes(m.identity_pub, pub)) throw new HttpError(409, "identity_mismatch");
        this.sql.exec(
          `UPDATE members SET joined = ?, identity_pub = ?, profile = COALESCE(?, profile) WHERE client_id = ?`,
          now, blob(pub), blob(profile), c.id,
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
            c.id, inv.role, blob(pub), blob(profile), now, now,
          );
          if (inv.uses_left !== null) this.sql.exec(`UPDATE invites SET uses_left = uses_left - 1 WHERE id = ?`, inv.id);
        });
      }
      await this.announce(c, "members", this.currentIDs(), { identity: { chain: c.id, pub } });
      return this.view(c, 200);
    });
  }

  private async updateMember(c: Caller, request: Request, client: string): Promise<Response> {
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
    let profile: Uint8Array | null = null;
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
        this.sql.exec(`UPDATE members SET profile = ? WHERE client_id = ?`, blob(profile), me.client_id);
      }
      await this.announce(c, "members", this.currentIDs());
      return noContent();
    });
  }

  private async removeMember(c: Caller, client: string): Promise<Response> {
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

  private async transfer(c: Caller, request: Request): Promise<Response> {
    const body = await readJSONBody(request, MAX_BODY);
    const id = fStr(body, "client_id").toLowerCase();
    if (!CHAIN_ID_RE.test(id)) throw new Bad("client_id must be 64 hex digits");
    return this.locked(async () => {
      const { room, me } = this.access(c.id, false, [ROLE_OWNER]);
      const m = this.currentTarget(id);
      if (m.joined === 0) throw new HttpError(403, "not_joined");
      if (m.client_id === me.client_id) return noContent();
      await this.moveBytes(room, m.client_id);
      this.ctx.storage.transactionSync(() => {
        this.sql.exec(`UPDATE room SET owner = ?`, m.client_id);
        this.sql.exec(`UPDATE members SET role = ? WHERE client_id = ?`, ROLE_EDITOR, me.client_id);
        this.sql.exec(`UPDATE members SET role = ? WHERE client_id = ?`, ROLE_OWNER, m.client_id);
      });
      if (room.bytes !== 0) await chainStub(this.env, room.owner).addRoomBytes(-room.bytes);
      await this.announce(c, "members", this.currentIDs());
      return noContent();
    });
  }

  // moveBytes counts the room's bytes against chain next (quota_exceeded if
  // they don't fit); the caller gives them back to the old owner after.
  private async moveBytes(room: RoomRow, next: string): Promise<void> {
    if (room.bytes > 0 && !(await chainStub(this.env, next).addRoomBytes(room.bytes))) throw quotaExceeded();
  }

  // successor moves the caller's membership (its role, the owner's room and
  // quota included) to the chain whose token was in X-Compass-Linked-Token
  // (the Worker checked it: X-Linked-Chain, empty if it was bad).
  private async successor(c: Caller, request: Request): Promise<Response> {
    const next = (request.headers.get("X-Linked-Chain") ?? "").toLowerCase();
    if (!next) throw new HttpError(400, "invalid_linked_token");
    if (next === c.id) throw new Bad("the successor is the caller's own chain");
    const body = await readJSONBody(request, MAX_BODY);
    const pub = fBytes(body, "identity_pub");
    const envs = fObjects(body, "envelopes").map((e) => ({ epoch: fInt(e, "epoch"), sealed: fBytes(e, "sealed") }));
    if (pub?.length !== IDENTITY_KEY_BYTES) throw new Bad("identity_pub must be 32 bytes");
    if (envs.length === 0 || envs.length > MAX_ENVELOPES) throw new Bad("envelopes must hold 1 to 1000 envelopes");
    const seen = new Set<number>();
    for (const e of envs) {
      if (seen.has(e.epoch)) throw new Bad("two envelopes for one epoch");
      if (e.sealed?.length !== ENVELOPE_BYTES) throw new Bad("sealed must be 80 bytes");
      seen.add(e.epoch);
    }
    if ((await chainStub(this.env, next).status()).disabled) throw new HttpError(409, "successor_disabled");
    return this.locked(async () => {
      const { room, me } = this.access(c.id, true);
      const there = this.member(next);
      if (there && there.removed === 0) throw new HttpError(409, "already_member");
      let current = false;
      for (const e of envs) {
        if (e.epoch < 1 || e.epoch > room.epoch) throw new HttpError(409, "stale_epoch");
        current ||= e.epoch === room.epoch;
      }
      if (!current) throw new HttpError(409, "stale_epoch");
      const owner = me.role === ROLE_OWNER;
      if (owner) await this.moveBytes(room, next);
      const now = Date.now();
      this.ctx.storage.transactionSync(() => {
        if (owner) this.sql.exec(`UPDATE room SET owner = ?`, next);
        this.sql.exec(
          `INSERT INTO members (client_id, role, identity_pub, profile, invited, joined, removed)
           VALUES (?, ?, ?, ?, ?, ?, 0)
           ON CONFLICT (client_id) DO UPDATE SET role = excluded.role, identity_pub = excluded.identity_pub,
             profile = excluded.profile, invited = excluded.invited, joined = excluded.joined, removed = 0`,
          next, me.role, blob(pub), blob(me.profile), me.invited, me.joined === 0 ? 0 : now,
        );
        this.sql.exec(`DELETE FROM envelopes WHERE client_id IN (?, ?)`, me.client_id, next);
        for (const e of envs) this.putEnvelope(next, e.epoch, e.sealed!);
        this.sql.exec(
          `UPDATE members SET removed = ?, role = CASE WHEN role = ? THEN ? ELSE role END WHERE client_id = ?`,
          now, ROLE_OWNER, ROLE_EDITOR, me.client_id,
        );
      });
      if (owner && room.bytes !== 0) await chainStub(this.env, room.owner).addRoomBytes(-room.bytes);
      this.wake(me.client_id);
      await this.announce(c, "members", [...this.currentIDs(), me.client_id], { identity: { chain: next, pub } });
      return noContent();
    });
  }

  private async rotate(c: Caller, request: Request): Promise<Response> {
    const body = await readJSONBody(request, MAX_BODY);
    const epoch = fInt(body, "epoch");
    const envs = fObjects(body, "envelopes").map((e) => ({ id: fStr(e, "client_id"), sealed: fBytes(e, "sealed") }));
    if (epoch < 2) throw new Bad("epoch must be 2 or more");
    const byID = new Map<string, Uint8Array>();
    for (const e of envs) {
      const id = e.id.toLowerCase();
      if (!CHAIN_ID_RE.test(id)) throw new Bad("client_id must be 64 hex digits");
      if (byID.has(id)) throw new Bad("two envelopes for " + id);
      if (e.sealed?.length !== ENVELOPE_BYTES) throw new Bad("sealed must be 80 bytes");
      byID.set(id, e.sealed);
    }
    return this.locked(async () => {
      const { room } = this.access(c.id, false, [ROLE_OWNER]);
      if (epoch !== room.epoch + 1) throw new HttpError(409, "stale_epoch");
      const members = this.currentIDs();
      const missing = members.filter((id) => !byID.has(id));
      const unknown = [...byID.keys()].filter((id) => !members.includes(id)).sort();
      if (missing.length > 0 || unknown.length > 0) {
        throw new HttpError(409, "envelopes_mismatch", { missing, unknown });
      }
      this.ctx.storage.transactionSync(() => {
        for (const id of members) this.putEnvelope(id, epoch, byID.get(id)!);
        this.sql.exec(`UPDATE room SET epoch = ?`, epoch);
      });
      await this.announce(c, "keys", members);
      return noContent();
    });
  }

  private async addEnvelopes(c: Caller, request: Request): Promise<Response> {
    const body = await readJSONBody(request, MAX_BODY);
    const envs = fObjects(body, "envelopes").map((e) => ({
      id: fStr(e, "client_id").toLowerCase(),
      epoch: fInt(e, "epoch"),
      sealed: fBytes(e, "sealed"),
    }));
    if (envs.length === 0 || envs.length > MAX_ENVELOPES) throw new Bad("envelopes must hold 1 to 1000 envelopes");
    for (const e of envs) {
      if (!CHAIN_ID_RE.test(e.id)) throw new Bad("client_id must be 64 hex digits");
      if (e.sealed?.length !== ENVELOPE_BYTES) throw new Bad("sealed must be 80 bytes");
    }
    return this.locked(async () => {
      const { room } = this.access(c.id, false, [ROLE_OWNER]);
      for (const e of envs) {
        if (e.epoch < 1 || e.epoch > room.epoch) throw new HttpError(409, "stale_epoch");
        this.currentTarget(e.id);
      }
      this.ctx.storage.transactionSync(() => {
        for (const e of envs) this.putEnvelope(e.id, e.epoch, e.sealed!);
      });
      await this.announce(c, "keys", [...new Set(envs.map((e) => e.id))].sort());
      return noContent();
    });
  }

  // --- Calls from other objects and the Worker -------------------------------

  // identityChanged records chain's new identity key in its membership, if
  // it's a current member with another key, and tells the members (PUT
  // /v2/compass/identity). device is the calling device.
  async identityChanged(roomID: string, chain: string, pub: string, device: string): Promise<void> {
    const key = decodeStd(pub);
    if (!key) return;
    await this.locked(async () => {
      const m = this.member(chain);
      if (!m || m.removed !== 0 || (m.identity_pub && equalBytes(m.identity_pub, key))) return;
      this.sql.exec(`UPDATE members SET identity_pub = ? WHERE client_id = ?`, blob(key), m.client_id);
      await this.announce({ id: m.client_id, listed: true, device, room: roomID }, "members", this.currentIDs());
    });
  }

  // chainGone is a chain's data being cleared (reset, idle expiry): a room
  // it owns is deleted, its membership of another goes with its envelopes.
  // Nothing is announced (sync.md, "Going away"); the other members' indexes
  // drop a deleted room.
  async chainGone(roomID: string, chainID: string): Promise<void> {
    const chain = chainID.toLowerCase();
    await this.locked(async () => {
      const room = this.roomRow();
      if (!room) return;
      if (room.owner === chain) {
        const others = this.currentIDs().filter((id) => id !== chain);
        this.wipe();
        this.wake();
        await this.announce({ id: chain, listed: true, device: "", room: roomID }, "deleted", others, {
          seq: room.seq,
          silent: true,
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
}

function inviteRow(r: Row): InviteRow {
  return {
    id: r.id as string,
    secret_hash: bytesOf(r.secret_hash) ?? new Uint8Array(0),
    role: r.role as string,
    created: Number(r.created),
    expires: Number(r.expires),
    uses_left: r.uses_left === null ? null : Number(r.uses_left),
    revoked: Number(r.revoked) !== 0,
  };
}

function inviteOut(inv: InviteRow) {
  return { id: inv.id, role: inv.role, created: inv.created, expires: inv.expires, uses_left: inv.uses_left, revoked: inv.revoked };
}

