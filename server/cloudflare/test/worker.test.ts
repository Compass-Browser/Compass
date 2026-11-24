// End-to-end checks of the Worker beyond the Go suite
// (server/server/e2e_test.go and e2e_rooms_test.go, which also run against
// it with COMPASS_SYNC_E2E_URL): the change stream, quota, device removal,
// the access list, and what the Go rooms tests can't check against a URL
// (room events, long polls, resets, successors, room bytes in the quota,
// the private server's gate for rooms). Run against `wrangler dev`:
//
//   COMPASS_CF_URL=http://127.0.0.1:8797 npm test
//
// With COMPASS_CF_OWNER_SEED (hex) set, the server is expected to have
// that seed's chain as its owner (COMPASS_SYNC_ALLOWED_CHAINS) and the
// access tests run; without it, the server must be open.

import assert from "node:assert/strict";
import { createHash, createPrivateKey, createPublicKey, randomBytes, sign } from "node:crypto";
import { test } from "node:test";
import {
  decodeSyncEntity,
  emptySpecifics,
  fields,
  Writer,
} from "../src/proto.ts";

const URL_ = process.env.COMPASS_CF_URL ?? "http://127.0.0.1:8797";
const OWNER_SEED = process.env.COMPASS_CF_OWNER_SEED ?? "";
const BOOKMARK = 32904;
const DEVICE_INFO = 154522;

class Chain {
  readonly key;
  readonly id: string;
  constructor(seed = randomBytes(32)) {
    // PKCS#8 wrapper for a raw Ed25519 seed.
    const der = Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), seed]);
    this.key = createPrivateKey({ key: der, format: "der", type: "pkcs8" });
    const spki = createPublicKey(this.key).export({ format: "der", type: "spki" }) as Buffer;
    this.id = spki.subarray(spki.length - 32).toString("hex").toUpperCase();
  }
  token(): string {
    const ts = Buffer.from(String(Date.now()));
    const sig = sign(null, ts, this.key);
    const raw = `${ts.toString("hex").toUpperCase()}|${sig.toString("hex").toUpperCase()}|${this.id}`;
    return Buffer.from(raw).toString("base64url").padEnd(Math.ceil(raw.length / 3) * 4, "=");
  }
  async call(method: string, path: string, body?: BodyInit | null, headers: Record<string, string> = {}) {
    return fetch(URL_ + path, {
      method,
      body,
      headers: { Authorization: "Bearer " + this.token(), ...headers },
    });
  }
}

// --- Rooms ---

const b64 = (b: Uint8Array) => Buffer.from(b).toString("base64");
const sha = (s: string | Uint8Array) => createHash("sha256").update(s).digest();
// An 80-byte stand-in for an HPKE envelope.
const envelope = (label: string) => b64(Buffer.concat([sha(label + "1"), sha(label + "2"), sha(label + "3")]).subarray(0, 80));
// A stand-in for a record: nonce (12) || text || tag (16).
const ct = (text: string) => b64(Buffer.concat([Buffer.alloc(12), Buffer.from(text), Buffer.alloc(16)]));
const identity = (c: Chain) => b64(sha("identity " + c.id));
const lc = (c: Chain) => c.id.toLowerCase();

async function api(
  c: Chain,
  method: string,
  path: string,
  body?: unknown,
  headers: Record<string, string> = {},
): Promise<{ status: number; body: Record<string, any> | null }> {
  const r = await c.call(method, path, body === undefined ? null : JSON.stringify(body), headers);
  const t = await r.text();
  return { status: r.status, body: t ? JSON.parse(t) : null };
}

async function must(c: Chain, status: number, method: string, path: string, body?: unknown, headers?: Record<string, string>) {
  const r = await api(c, method, path, body, headers);
  assert.equal(r.status, status, `${method} ${path}: ${JSON.stringify(r.body)}`);
  return r.body!;
}

async function createRoom(c: Chain, kind = "folder"): Promise<string> {
  const out = await must(c, 201, "POST", "/v2/compass/rooms", {
    kind,
    identity_pub: identity(c),
    envelope: envelope("1-" + c.id),
  });
  return out.id as string;
}

// invite makes a link invite and returns what joining needs.
async function invite(owner: Chain, room: string, role: string) {
  const secret = randomBytes(32);
  const out = await must(owner, 201, "POST", `/v2/compass/rooms/${room}/invites`, {
    role,
    secret_hash: sha(secret).toString("hex"),
  });
  return { invite_id: out.id as string, secret: secret.toString("base64url") };
}

function join(c: Chain, room: string, inv: { invite_id: string; secret: string }) {
  return api(c, "POST", `/v2/compass/rooms/${room}/join`, { ...inv, identity_pub: identity(c) });
}

// shared: a owns a folder room, b joined it as editor.
async function shared(a: Chain, b: Chain): Promise<string> {
  const room = await createRoom(a);
  const r = await join(b, room, await invite(a, room, "editor"));
  assert.equal(r.status, 200, JSON.stringify(r.body));
  return room;
}

function write(c: Chain, room: string, device: string, records: Record<string, unknown>[], epoch = 1) {
  return api(c, "POST", `/v2/compass/rooms/${room}/records?client_id=${device}`, {
    records: records.map((r) => ({ epoch, ...r })),
  });
}

async function quotaBytes(c: Chain): Promise<number> {
  const r = await c.call("GET", "/v2/compass/quota");
  assert.equal(r.status, 200);
  return ((await r.json()) as { bytes: number }).bytes;
}

function getUpdates(types: number[], origin = 9): Uint8Array {
  const gu = new Writer().bool(3, true).int(9, origin);
  for (const t of types) gu.message(6, new Writer().int(1, t));
  return new Writer().string(1, "x").int(3, 2).message(5, gu).finish();
}

function commit(cacheGuid: string, entries: Writer[]): Uint8Array {
  const c = new Writer();
  for (const e of entries) c.message(1, e);
  c.string(2, cacheGuid);
  return new Writer().string(1, "x").int(3, 1).message(4, c).finish();
}

function bookmark(id: string, parent: string, title: string, version = 0): Writer {
  return new Writer()
    .string(1, id)
    .string(2, parent)
    .int(4, version)
    .string(8, title)
    .bytes(21, emptySpecifics(BOOKMARK))
    .bool(22, false);
}

interface Rsp {
  errorCode?: number;
  entries: ReturnType<typeof decodeSyncEntity>[];
  commitResults: { type: number; id?: string; version?: number }[];
}

async function command(chain: Chain, client: string, msg: Uint8Array): Promise<Rsp> {
  const r = await chain.call("POST", `/v2/command/?client_id=${encodeURIComponent(client)}`, msg);
  assert.equal(r.status, 200, await r.clone().text());
  const buf = new Uint8Array(await r.arrayBuffer());
  const out: Rsp = { entries: [], commitResults: [] };
  for (const f of fields(buf)) {
    if (f.num === 4) out.errorCode = f.int;
    if (f.num === 2) for (const g of fields(f.bytes)) if (g.num === 1) out.entries.push(decodeSyncEntity(g.bytes));
    if (f.num === 1) {
      for (const g of fields(f.bytes)) {
        const e: { type: number; id?: string; version?: number } = { type: 0 };
        for (const h of fields(g.bytes)) {
          if (h.num === 2) e.type = h.int;
          if (h.num === 3) e.id = new TextDecoder().decode(h.bytes);
          if (h.num === 6) e.version = h.int;
        }
        out.commitResults.push(e);
      }
    }
  }
  return out;
}

// stream opens the change stream and collects its text.
async function stream(chain: Chain, client: string) {
  const ctrl = new AbortController();
  const r = await fetch(`${URL_}/v2/compass/events?client_id=${client}`, {
    headers: { Authorization: "Bearer " + chain.token() },
    signal: ctrl.signal,
  });
  assert.equal(r.status, 200);
  assert.equal(r.headers.get("content-type"), "text/event-stream");
  let text = "";
  const reader = r.body!.getReader();
  (async () => {
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        text += new TextDecoder().decode(value);
      }
    } catch {
      // Aborted.
    }
  })();
  return {
    async waitFor(s: string, ms = 5000) {
      const end = Date.now() + ms;
      while (!text.includes(s)) {
        if (Date.now() > end) throw new Error(`no ${JSON.stringify(s)} in ${JSON.stringify(text)}`);
        await new Promise((r) => setTimeout(r, 50));
      }
    },
    get text() {
      return text;
    },
    close: () => ctrl.abort(),
  };
}

async function setUp(chain: Chain, client: string) {
  const rsp = await command(chain, client, getUpdates([BOOKMARK, 47745]));
  assert.equal(rsp.errorCode, 0);
  const bar = rsp.entries.find((e) => e.serverDefinedUniqueTag === "bookmark_bar");
  assert.ok(bar, "bookmark bar");
  return bar!;
}

test("open server", { skip: OWNER_SEED !== "" }, async (t) => {
  await t.test("info names the server and its version", async () => {
    const r = await fetch(URL_ + "/v2/compass/info");
    const info = (await r.json()) as { login: unknown; server: { kind: string; version: number } };
    assert.equal(info.login, null);
    assert.equal(info.server.kind, "cloudflare");
    assert.ok(info.server.version >= 2);
  });

  await t.test("change stream: another device's commit is announced", async () => {
    const chain = new Chain();
    const bar = await setUp(chain, "dev-a");
    const a = await stream(chain, "dev-a");
    const b = await stream(chain, "dev-b");
    await a.waitFor(": connected");
    await b.waitFor(": connected");
    const rsp = await command(chain, "dev-a", commit("dev-a", [bookmark("l1", bar.idString!, "one")]));
    assert.equal(rsp.commitResults[0].type, 1);
    await b.waitFor(`event: changed\ndata: {"types":[${BOOKMARK}]}`);
    await new Promise((r) => setTimeout(r, 300));
    assert.ok(!a.text.includes("event: changed"), "the committer isn't told");
    a.close();
    b.close();
  });

  await t.test("a quiet stream keeps alive and still hears a change", { skip: !process.env.COMPASS_CF_SLOW }, async () => {
    const chain = new Chain();
    const bar = await setUp(chain, "dev-a");
    const b = await stream(chain, "dev-b");
    await b.waitFor(": connected");
    // Long enough for the chain's object to hibernate (about 10 s idle)
    // and for three keep-alives.
    await new Promise((r) => setTimeout(r, 80_000));
    assert.ok(b.text.split(": keep-alive").length - 1 >= 3, b.text);
    await command(chain, "dev-a", commit("dev-a", [bookmark("l9", bar.idString!, "late")]));
    await b.waitFor(`event: changed`);
    b.close();
  });

  await t.test("quota counts entities and bytes", async () => {
    const chain = new Chain();
    const bar = await setUp(chain, "dev-a");
    await command(chain, "dev-a", commit("dev-a", [bookmark("l1", bar.idString!, "one"), bookmark("l2", bar.idString!, "two")]));
    const r = await chain.call("GET", "/v2/compass/quota");
    assert.equal(r.status, 200);
    const q = (await r.json()) as Record<string, unknown>;
    assert.equal(q.entities, 2);
    assert.ok((q.bytes as number) > 0);
    assert.deepEqual(q.full, []);
  });

  await t.test("a removed device is refused", async () => {
    const chain = new Chain();
    await setUp(chain, "dev-a");
    await setUp(chain, "dev-b");
    const s = await stream(chain, "dev-c");
    let r = await chain.call("DELETE", "/v2/compass/devices/dev-b?client_id=dev-a");
    assert.equal(r.status, 204);
    await s.waitFor(`{"types":[${DEVICE_INFO}]}`);
    s.close();
    r = await chain.call("POST", "/v2/command/?client_id=dev-b", getUpdates([BOOKMARK], 4));
    assert.equal(r.status, 403);
    assert.equal(r.headers.get("x-compass-device"), "removed");
    r = await chain.call("DELETE", "/v2/compass/devices/dev-a?client_id=dev-a");
    assert.equal(r.status, 409);
  });

  await t.test("reset disables the chain", async () => {
    const chain = new Chain();
    await setUp(chain, "dev-a");
    const clear = new Writer().string(1, "x").int(3, 5).message(15, new Writer()).finish();
    await command(chain, "dev-a", clear);
    const rsp = await command(chain, "dev-a", getUpdates([BOOKMARK], 4));
    assert.equal(rsp.errorCode, 10); // DISABLED_BY_ADMIN
  });
});

test("open server: rooms", { skip: OWNER_SEED !== "" }, async (t) => {
  await t.test("a write is a room event on the change stream, but the writing device's", async () => {
    const a = new Chain();
    const b = new Chain();
    const room = await shared(a, b);
    const sa = await stream(a, "a-1");
    const sb = await stream(b, "b-1");
    await sa.waitFor(": connected");
    await sb.waitFor(": connected");
    const w = await write(a, room, "a-1", [{ id: "node:1", ciphertext: ct("hello") }]);
    assert.equal(w.status, 200, JSON.stringify(w.body));
    await sb.waitFor(`event: room\ndata: {"room_id":"${room}","seq":1,"changes":["records"]}\n\n`);
    await new Promise((r) => setTimeout(r, 300));
    assert.ok(!sa.text.includes("event: room"), "the writing device isn't told");
    // A device of the same chain that didn't write is.
    const sa2 = await stream(a, "a-2");
    await sa2.waitFor(": connected");
    assert.equal((await write(b, room, "b-1", [{ id: "node:2", ciphertext: ct("x") }])).status, 200);
    await sa2.waitFor(`data: {"room_id":"${room}","seq":2,"changes":["records"]}`);
    await sa.waitFor(`data: {"room_id":"${room}","seq":2,"changes":["records"]}`);
    // Members: the removed member hears of its removal.
    await must(a, 204, "DELETE", `/v2/compass/rooms/${room}/members/${lc(b)}?client_id=a-1`);
    await sb.waitFor(`data: {"room_id":"${room}","seq":2,"changes":["members"]}`);
    for (const s of [sa, sa2, sb]) s.close();
  });

  await t.test("a long poll waits, and wakes on a write", async () => {
    const a = new Chain();
    const b = new Chain();
    const room = await shared(a, b);
    let start = Date.now();
    let out = await must(b, 200, "GET", `/v2/compass/rooms/${room}/changes?since=0&wait=1`);
    assert.ok(Date.now() - start >= 900, "waited");
    assert.deepEqual(out, { seq: 0, epoch: 1, records: [], more: false });
    const poll = must(b, 200, "GET", `/v2/compass/rooms/${room}/changes?since=0&wait=25`);
    await new Promise((r) => setTimeout(r, 500));
    start = Date.now();
    assert.equal((await write(a, room, "a-1", [{ id: "r", ciphertext: ct("x") }])).status, 200);
    out = await poll;
    assert.ok(Date.now() - start < 5000, "woken at once");
    assert.equal(out.seq, 1);
    assert.equal(out.records.length, 1);
    assert.equal(out.records[0].ciphertext, ct("x"));
    // Something newer already: answered at once.
    start = Date.now();
    out = await must(b, 200, "GET", `/v2/compass/rooms/${room}/changes?since=0&wait=25`);
    assert.ok(Date.now() - start < 3000);
    assert.equal(out.records.length, 1);
    // A removal ends it with 403 removed.
    const removed = api(b, "GET", `/v2/compass/rooms/${room}/changes?since=1&wait=25`);
    await new Promise((r) => setTimeout(r, 500));
    await must(a, 204, "DELETE", `/v2/compass/rooms/${room}/members/${lc(b)}`);
    const r = await removed;
    assert.equal(r.status, 403);
    assert.equal(r.body!.error, "removed");
  });

  await t.test("a room's bytes count in its owner's quota, and move with it", async () => {
    const a = new Chain();
    const b = new Chain();
    const room = await shared(a, b);
    const text = "x".repeat(100);
    assert.equal((await write(b, room, "b-1", [{ id: "r1", ciphertext: ct(text) }])).status, 200);
    const size = 12 + 100 + 16 + 2;
    assert.equal(await quotaBytes(a), size);
    assert.equal(await quotaBytes(b), 0, "not the author's");
    assert.equal((await must(a, 200, "GET", `/v2/compass/rooms/${room}`)).bytes, size);
    await must(a, 204, "POST", `/v2/compass/rooms/${room}/owner`, { client_id: lc(b) });
    assert.equal(await quotaBytes(a), 0);
    assert.equal(await quotaBytes(b), size);
    assert.equal((await write(b, room, "b-1", [{ id: "r1", deleted: true }])).status, 200);
    assert.equal(await quotaBytes(b), 2, "a tombstone keeps its ID");
    await must(b, 204, "DELETE", `/v2/compass/rooms/${room}`);
    assert.equal(await quotaBytes(b), 0);
  });

  await t.test("a chain reset deletes the rooms it owns and leaves the others", async () => {
    const a = new Chain();
    const b = new Chain();
    const owned = await shared(a, b);
    const other = await shared(b, a);
    const sb = await stream(b, "b-1");
    await sb.waitFor(": connected");
    const clear = new Writer().string(1, "x").int(3, 5).message(15, new Writer()).finish();
    await command(a, "a-1", clear);
    let r = await api(b, "GET", `/v2/compass/rooms/${owned}`);
    assert.equal(r.status, 404);
    assert.equal(r.body!.error, "not_found");
    const list = await must(b, 200, "GET", "/v2/compass/rooms");
    assert.deepEqual(list.rooms.map((x: { id: string }) => x.id), [other]);
    const view = await must(b, 200, "GET", `/v2/compass/rooms/${other}`);
    assert.deepEqual(view.members.map((m: { client_id: string }) => m.client_id), [lc(b)]);
    r = await api(a, "GET", "/v2/compass/rooms");
    assert.equal(r.status, 403);
    assert.equal(r.body!.error, "chain_disabled");
    await new Promise((res) => setTimeout(res, 300));
    assert.ok(!sb.text.includes("event: room"), "nothing is announced");
    sb.close();
  });

  await t.test("successors take a reset chain's memberships", async () => {
    const a = new Chain();
    const b = new Chain();
    const a2 = new Chain();
    const b2 = new Chain();
    const room = await shared(a, b);
    assert.equal((await write(b, room, "b-1", [{ id: "x", ciphertext: ct("before") }])).status, 200);
    const body = (c: Chain) => ({ identity_pub: identity(c), envelopes: [{ epoch: 1, sealed: envelope("for " + c.id) }] });
    let r = await api(a, "POST", `/v2/compass/rooms/${room}/successor`, body(a2), { "X-Compass-Linked-Token": "nope" });
    assert.equal(r.status, 400);
    assert.deepEqual(r.body, { error: "invalid_linked_token" });
    r = await api(a, "POST", `/v2/compass/rooms/${room}/successor`, body(a), { "X-Compass-Linked-Token": a.token() });
    assert.equal(r.status, 400, "its own chain");
    r = await api(a, "POST", `/v2/compass/rooms/${room}/successor`, body(b), { "X-Compass-Linked-Token": b.token() });
    assert.equal(r.status, 409);
    assert.equal(r.body!.error, "already_member");

    const sb = await stream(b, "b-1");
    await sb.waitFor(": connected");
    await must(a, 204, "POST", `/v2/compass/rooms/${room}/owner`, body(a2), { "X-Compass-Linked-Token": a2.token() });
    await sb.waitFor(`data: {"room_id":"${room}","seq":1,"changes":["members"]}`);
    sb.close();
    await must(b, 204, "POST", `/v2/compass/rooms/${room}/successor`, body(b2), {
      "X-Compass-Linked-Token": "Bearer " + b2.token(),
    });

    const view = await must(a2, 200, "GET", `/v2/compass/rooms/${room}`);
    assert.equal(view.owner, lc(a2));
    assert.equal(view.role, "owner");
    assert.deepEqual(view.envelopes, [{ epoch: 1, sealed: envelope("for " + a2.id) }]);
    assert.deepEqual((await must(a2, 200, "GET", "/v2/compass/identity")).identity_pub, identity(a2));
    assert.deepEqual((await must(a2, 200, "GET", "/v2/compass/rooms")).rooms, [
      { id: room, kind: "folder", owner: lc(a2), role: "owner", pending: false, epoch: 1, seq: 1 },
    ]);
    assert.equal((await must(b2, 200, "GET", `/v2/compass/rooms/${room}`)).role, "editor");
    assert.equal((await must(b2, 200, "GET", `/v2/compass/rooms/${room}/changes`)).records.length, 1);
    for (const old of [a, b]) {
      r = await api(old, "GET", `/v2/compass/rooms/${room}`);
      assert.equal(r.status, 403);
      assert.equal(r.body!.error, "removed");
      assert.deepEqual((await must(old, 200, "GET", "/v2/compass/rooms")).rooms, []);
    }
    const size = 12 + 6 + 16 + 1;
    assert.equal(await quotaBytes(a), 0);
    assert.equal(await quotaBytes(a2), size, "the bytes moved with the room");
  });

  await t.test("an identity key change reaches the memberships", async () => {
    const a = new Chain();
    const b = new Chain();
    const room = await shared(a, b);
    const sa = await stream(a, "a-1");
    await sa.waitFor(": connected");
    const key = b64(sha("new key"));
    await must(b, 204, "PUT", "/v2/compass/identity", { identity_pub: key });
    assert.equal((await must(b, 200, "GET", "/v2/compass/identity")).identity_pub, key);
    await sa.waitFor(`data: {"room_id":"${room}","seq":0,"changes":["members"]}`);
    sa.close();
    const view = await must(a, 200, "GET", `/v2/compass/rooms/${room}`);
    assert.equal(view.members.find((m: { client_id: string }) => m.client_id === lc(b)).identity_pub, key);
  });
});

test("private server", { skip: OWNER_SEED === "" }, async (t) => {
  const owner = new Chain(Buffer.from(OWNER_SEED, "hex"));
  const friend = new Chain();
  const stranger = new Chain();
  const profile = new Chain();

  await t.test("the owner syncs, a stranger doesn't", async () => {
    await setUp(owner, "o-1");
    const r = await stranger.call("POST", "/v2/command/", getUpdates([BOOKMARK]));
    assert.equal(r.status, 401);
  });

  await t.test("a friend joins with an invite, once", async () => {
    let r = await owner.call("POST", "/v2/compass/access/invites", JSON.stringify({ name: "Sam" }));
    assert.equal(r.status, 201);
    const { code, link } = (await r.json()) as { code: string; link: string };
    assert.equal(link, `${URL_}/invite#${code}`);
    const page = await fetch(link);
    assert.equal(page.status, 200);
    assert.match(await page.text(), /I have an invite/);
    r = await friend.call("POST", "/v2/compass/access/join", JSON.stringify({ code }));
    assert.equal(r.status, 204);
    await setUp(friend, "f-1");
    r = await stranger.call("POST", "/v2/compass/access/join", JSON.stringify({ code }));
    assert.equal(r.status, 410);
    r = await friend.call("POST", "/v2/compass/access/invites", "{}");
    assert.equal(r.status, 403, "friends don't invite");
  });

  await t.test("a friend's profile account passes once linked", async () => {
    let r = await profile.call("POST", "/v2/command/", getUpdates([BOOKMARK]));
    assert.equal(r.status, 401);
    r = await friend.call("POST", "/v2/compass/account/link", null, {
      "X-Compass-Linked-Token": profile.token(),
    });
    assert.equal(r.status, 204);
    await setUp(profile, "p-1");
  });

  await t.test("the owner lists and removes friends", async () => {
    let r = await owner.call("GET", "/v2/compass/access");
    const list = (await r.json()) as { members: { chain: string; name: string; linked: number }[] };
    const m = list.members.find((x) => x.chain === friend.id.toLowerCase());
    assert.ok(m);
    assert.equal(m!.name, "Sam");
    assert.equal(m!.linked, 1);
    r = await owner.call("DELETE", `/v2/compass/access/${friend.id.toLowerCase()}`);
    assert.equal(r.status, 204);
  });

  await t.test("rooms: a chain the list doesn't name uses only the rooms it's in", async () => {
    const stranger = new Chain();
    const room = await createRoom(owner, "space");
    const inv = await invite(owner, room, "editor");
    const notAllowed = { error: "chain_not_allowed" };
    for (const [method, path] of [
      ["POST", "/v2/compass/rooms"],
      ["GET", "/v2/compass/rooms"],
      ["GET", `/v2/compass/rooms/${room}`],
      ["GET", `/v2/compass/rooms/${room}/changes`],
      ["PUT", "/v2/compass/identity"],
      ["GET", "/v2/compass/identity"],
      ["GET", "/v2/compass/directory?email=a@example.com"],
    ]) {
      const r = await api(stranger, method, path, method === "GET" ? undefined : {});
      assert.equal(r.status, 403, path);
      assert.deepEqual(r.body, notAllowed, path);
    }
    let r = await join(stranger, room, { invite_id: inv.invite_id, secret: randomBytes(32).toString("base64url") });
    assert.equal(r.status, 403);
    assert.deepEqual(r.body, notAllowed);
    r = await join(stranger, room, inv);
    assert.equal(r.status, 200, JSON.stringify(r.body));

    await must(stranger, 200, "GET", `/v2/compass/rooms/${room}`);
    await must(stranger, 200, "GET", "/v2/compass/rooms");
    await must(stranger, 200, "GET", `/v2/compass/rooms/${room}/changes?wait=1`);
    assert.equal((await write(stranger, room, "s-1", [{ id: "x", ciphertext: ct("y") }])).status, 200);
    await must(stranger, 204, "PUT", "/v2/compass/identity", { identity_pub: identity(stranger) });
    assert.equal((await must(stranger, 200, "GET", "/v2/compass/identity")).identity_pub, identity(stranger));
    r = await api(stranger, "POST", "/v2/compass/rooms", { kind: "folder", identity_pub: identity(stranger), envelope: envelope("x") });
    assert.equal(r.status, 403);
    assert.deepEqual(r.body, notAllowed);
    assert.equal((await stranger.call("POST", "/v2/command/", getUpdates([BOOKMARK]))).status, 401, "personal sync still refused");
    assert.equal((await stranger.call("GET", "/v2/compass/quota")).status, 401);
    const other = await createRoom(owner);
    r = await api(stranger, "GET", `/v2/compass/rooms/${other}`);
    assert.equal(r.status, 403);
    assert.deepEqual(r.body, notAllowed, "only the rooms it's in");

    // Its successor (a reset) isn't listed either, and needs no place in the list.
    const next = new Chain();
    await must(stranger, 204, "POST", `/v2/compass/rooms/${room}/successor`,
      { identity_pub: identity(next), envelopes: [{ epoch: 1, sealed: envelope("n") }] },
      { "X-Compass-Linked-Token": next.token() });
    assert.equal((await must(next, 200, "GET", `/v2/compass/rooms/${room}/changes`)).records.length, 1);
    assert.equal((await next.call("POST", "/v2/command/", getUpdates([BOOKMARK]))).status, 401);
    r = await api(stranger, "GET", `/v2/compass/rooms/${room}`);
    assert.equal(r.status, 403);
    assert.equal(r.body!.error, "removed");
    r = await api(stranger, "GET", "/v2/compass/rooms");
    assert.equal(r.status, 403);
    assert.deepEqual(r.body, notAllowed, "no room left");

    // Removed: it's told so; then it has no room left.
    await must(owner, 204, "DELETE", `/v2/compass/rooms/${room}/members/${lc(next)}`);
    r = await api(next, "GET", `/v2/compass/rooms/${room}`);
    assert.equal(r.status, 403);
    assert.equal(r.body!.error, "removed");
    r = await api(next, "GET", "/v2/compass/rooms");
    assert.equal(r.status, 403);
    assert.deepEqual(r.body, notAllowed);
  });
});
