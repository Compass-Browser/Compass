// End-to-end checks of the Worker beyond the Go suite
// (server/server/e2e_test.go, which also runs against it with
// COMPASS_SYNC_E2E_URL): the change stream, quota, device removal and the
// access list. Run against `wrangler dev`:
//
//   COMPASS_CF_URL=http://127.0.0.1:8797 npm test
//
// With COMPASS_CF_OWNER_SEED (hex) set, the server is expected to have
// that seed's chain as its owner (COMPASS_SYNC_ALLOWED_CHAINS) and the
// access tests run; without it, the server must be open.

import assert from "node:assert/strict";
import { createPrivateKey, createPublicKey, randomBytes, sign } from "node:crypto";
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
});
