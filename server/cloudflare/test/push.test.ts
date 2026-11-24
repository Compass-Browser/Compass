// Push through the relay (src/push.ts): runs the server under `wrangler
// dev` against a mock relay that records what it's asked, and checks which
// devices are pushed to, when, and what the server keeps.
//
//   node --test test/push.test.ts

import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import { createPrivateKey, createPublicKey, randomBytes, sign } from "node:crypto";
import { mkdtempSync, rmSync } from "node:fs";
import { createServer, type Server } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";
import { emptySpecifics, Writer } from "../src/proto.ts";

const TOPIC = "it.cipi1965.compass";
const SEND_TAB = 601980;
const SPACES = 970000;
const BOOKMARK = 32904;

interface Call {
  path: string;
  body: Record<string, string>;
}

let calls: Call[] = [];
let goneHandles = new Set<string>();
let relayDown = false;
let mock: Server;
let server: ChildProcess;
let URL_ = "";
let dir = "";

class Chain {
  readonly key;
  readonly id: string;
  constructor() {
    const der = Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), randomBytes(32)]);
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
  call(method: string, path: string, body?: BodyInit | null) {
    return fetch(URL_ + path, { method, body, headers: { Authorization: "Bearer " + this.token() } });
  }
  async register(guid: string, reg: Record<string, string>) {
    return (await this.call("POST", `/v2/compass/push?client_id=${guid}`, JSON.stringify(reg))).status;
  }
  async commit(guid: string, type: number, id = randomBytes(8).toString("hex")) {
    const e = new Writer().string(1, id).int(4, 0).string(8, "x").bytes(21, emptySpecifics(type)).bool(22, false);
    if (type === BOOKMARK) e.string(2, "bookmark_bar");
    else e.string(23, randomBytes(20).toString("base64"));
    const c = new Writer().message(1, e).string(2, guid);
    const msg = new Writer().string(1, "x").int(3, 1).message(4, c).finish();
    const r = await this.call("POST", `/v2/command/?client_id=${encodeURIComponent(guid)}`, msg);
    assert.equal(r.status, 200);
    await r.arrayBuffer();
  }
}

const iphone = (n: number) => ({ token: n.toString(16).padStart(2, "0").repeat(32), environment: "production", topic: TOPIC });
const android = (n: number) => ({ platform: "fcm", token: `fcm${n}:` + "a".repeat(60), topic: TOPIC });

// The handle the mock gives a token.
const handleOf = (token: string) => `h1.${Buffer.from(token).toString("base64url")}`;
const tokenOf = (handle: string) => Buffer.from(handle.slice(3), "base64url").toString();

// The server's owners (COMPASS_SYNC_ALLOWED_CHAINS): every test's chain is
// one, so the access list's settings can be changed.
const owners = Array.from({ length: 8 }, () => new Chain());
const owner = () => owners.pop()!;

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// pushes waits a moment (they go out after the commit answers) and returns
// the pushes since the last call, as "type token" sorted.
async function pushes(wait = 400): Promise<string[]> {
  await sleep(wait);
  const out = calls.filter((c) => c.path === "/v1/push").map((c) => `${c.body.type} ${tokenOf(c.body.handle)}`);
  calls = [];
  return out.sort();
}

before(async () => {
  mock = createServer((req, res) => {
    let raw = "";
    req.on("data", (d) => (raw += d));
    req.on("end", () => {
      const body = JSON.parse(raw || "{}");
      calls.push({ path: req.url ?? "", body });
      res.setHeader("content-type", "application/json");
      if (relayDown) {
        res.writeHead(502).end(`{"error":"upstream"}`);
      } else if (req.url === "/v1/seal") {
        res.writeHead(200).end(JSON.stringify({ handle: handleOf(body.token) }));
      } else if (goneHandles.has(body.handle)) {
        res.writeHead(410).end(`{"error":"gone"}`);
      } else {
        res.writeHead(200).end(`{"result":"sent"}`);
      }
    });
  });
  await new Promise<void>((r) => mock.listen(0, "127.0.0.1", r));
  const relay = `http://127.0.0.1:${(mock.address() as { port: number }).port}`;
  dir = mkdtempSync(join(tmpdir(), "push-test-"));
  const port = 20000 + Math.floor(Math.random() * 20000);
  URL_ = `http://127.0.0.1:${port}`;
  server = spawn(
    "npx",
    ["wrangler", "dev", "--port", String(port), "--ip", "127.0.0.1", "--persist-to", join(dir, "state"),
      "--var", `COMPASS_SYNC_PUSH_RELAY:${relay}`, "--var", "COMPASS_SYNC_PUSH_BACKGROUND_SECONDS:2",
      "--var", `COMPASS_SYNC_ALLOWED_CHAINS:${owners.map((c) => c.id).join(",")}`],
    { stdio: ["ignore", "pipe", "pipe"] },
  );
  let out = "";
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`wrangler dev didn't start:\n${out}`)), 60_000);
    const onData = (d: Buffer) => {
      out += d;
      if (out.includes("Ready on")) {
        clearTimeout(timer);
        resolve();
      }
    };
    server.stdout!.on("data", onData);
    server.stderr!.on("data", onData);
  });
});

after(() => {
  server?.kill();
  mock?.close();
  if (dir) rmSync(dir, { recursive: true, force: true });
});

test("registering seals the token at the relay, for this server", async () => {
  const c = owner();
  assert.equal(await c.register("phone", iphone(1)), 204);
  const seal = calls.find((x) => x.path === "/v1/seal")!;
  assert.deepEqual(seal.body, { server: new URL(URL_).host, platform: "apns", ...iphone(1) });
  calls = [];
  // Checked as the Go server does, before the relay is asked.
  for (const bad of [
    { ...iphone(1), topic: "com.example.other" },
    { ...iphone(1), token: "nothex" },
    { ...iphone(1), environment: "staging" },
    { ...android(1), token: "short" },
    { ...iphone(1), platform: "webpush" },
  ]) {
    assert.equal(await c.register("phone", bad), 400, JSON.stringify(bad));
  }
  assert.equal((await c.call("POST", "/v2/compass/push", JSON.stringify(iphone(1)))).status, 400); // no client_id
  assert.equal((await c.call("GET", "/v2/compass/push?client_id=phone")).status, 405);
  assert.equal(calls.length, 0);
  // The relay down: the device is told to register again later.
  relayDown = true;
  assert.equal(await c.register("phone2", iphone(2)), 503);
  relayDown = false;
  calls = [];
});

test("a sent tab alerts every other device, iPhone and Android", async () => {
  const c = owner();
  await c.register("phone", iphone(3));
  await c.register("droid", android(3));
  await c.register("mac", iphone(4)); // the sender: not alerted
  calls = [];
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), [`send-tab ${iphone(3).token}`, `send-tab ${android(3).token}`]);
  // Other types, and changes to an existing sent tab, alert no one.
  await c.commit("mac", BOOKMARK);
  assert.deepEqual(await pushes(), []);
});

test("background sync: iPhones only, spaced, not while Compass is open", async () => {
  const c = owner();
  await c.register("phone", iphone(5));
  await c.register("droid", android(5));
  calls = [];
  await c.commit("mac", SPACES);
  assert.deepEqual(await pushes(), [`sync ${iphone(5).token}`]);
  // Within the interval: held, then sent once for both changes.
  await c.commit("mac", SPACES);
  await c.commit("mac", SPACES);
  assert.deepEqual(await pushes(), []);
  assert.deepEqual(await pushes(2500), [`sync ${iphone(5).token}`]);
  // With the phone's change stream open, none.
  await sleep(2100);
  const ctrl = new AbortController();
  const stream = await fetch(`${URL_}/v2/compass/events?client_id=phone`, {
    headers: { Authorization: "Bearer " + c.token() },
    signal: ctrl.signal,
  });
  assert.equal(stream.status, 200);
  await sleep(300);
  await c.commit("mac", SPACES);
  assert.deepEqual(await pushes(), []);
  ctrl.abort();
});

test("gone devices, unregistering and removed devices stop pushes", async () => {
  const c = owner();
  await c.register("a", iphone(6));
  await c.register("b", iphone(7));
  await c.register("d", iphone(8));
  goneHandles.add(handleOf(iphone(6).token));
  calls = [];
  await c.commit("mac", SEND_TAB);
  assert.equal((await pushes()).length, 3);
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), [`send-tab ${iphone(7).token}`, `send-tab ${iphone(8).token}`]);
  assert.equal((await c.call("DELETE", "/v2/compass/push?client_id=b")).status, 204);
  assert.equal((await c.call("DELETE", "/v2/compass/devices/d?client_id=mac")).status, 204);
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), []);
});

test("a device that registers again under a new cache GUID is pushed to once", async () => {
  const c = owner();
  await c.register("old", iphone(9));
  await c.register("new", iphone(9));
  calls = [];
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), [`send-tab ${iphone(9).token}`]);
});

test("the owners can turn push off, which forgets the handles", async () => {
  const c = owner();
  const settings = async () => ((await (await c.call("GET", "/v2/compass/access")).json()) as { settings: unknown }).settings;
  const relay = "http://127.0.0.1";
  const first = (await settings()) as { push_relay: boolean; push_relay_url: string };
  assert.equal(first.push_relay, true);
  assert.ok(first.push_relay_url.startsWith(relay), first.push_relay_url);
  await c.register("phone", iphone(10));
  const put = (on: unknown) => c.call("PUT", "/v2/compass/access/settings", JSON.stringify({ push_relay: on }));
  assert.equal((await put("yes")).status, 400);
  const off = await put(false);
  assert.equal(off.status, 200);
  assert.equal(((await off.json()) as { push_relay: boolean }).push_relay, false);
  calls = [];
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), []);
  // Registering keeps nothing and asks the relay nothing.
  assert.equal(await c.register("phone", iphone(10)), 204);
  assert.equal(calls.length, 0);
  // On again: the old handle is gone; a new registration works.
  assert.equal((await put(true)).status, 200);
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), []);
  await c.register("phone", iphone(10));
  await c.commit("mac", SEND_TAB);
  assert.deepEqual(await pushes(), [`send-tab ${iphone(10).token}`]);
  // Only owners: a chain not on the list is refused.
  const stranger = new Chain();
  assert.equal((await stranger.call("PUT", "/v2/compass/access/settings", JSON.stringify({ push_relay: false }))).status, 401);
});
