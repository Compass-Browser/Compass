// The relay against mock APNs, Google OAuth and FCM endpoints that check
// its signatures: it starts them and `wrangler dev` itself.
//
//   npm run test:relay

import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import { createPublicKey, generateKeyPairSync, randomBytes, verify, type KeyObject } from "node:crypto";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { createServer, type IncomingMessage, type Server } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";

const TOPIC = "it.cipi1965.compass";
const SERVER = "compass-sync.example.workers.dev";
const GONE_TOKEN = "ab".repeat(32);

interface Seen {
  path: string;
  headers: IncomingMessage["headers"];
  body: string;
}

const apnsKeys = generateKeyPairSync("ec", { namedCurve: "P-256" });
const googleKeys = generateKeyPairSync("rsa", { modulusLength: 2048 });
const seen: Seen[] = [];
let mock: Server;
let mockURL = "";
let relay: ChildProcess;
let relayURL = "";
let dir = "";

function b64urlJSON(part: string): Record<string, unknown> {
  return JSON.parse(Buffer.from(part, "base64url").toString());
}

// checkJWT verifies a compact JWS with |key| (ES256 as raw r||s, or RS256)
// and returns its header and claims.
function checkJWT(jwt: string, key: KeyObject, alg: "ES256" | "RS256") {
  const [h, c, s] = jwt.split(".");
  const header = b64urlJSON(h);
  assert.equal(header.alg, alg);
  const ok = verify(
    "sha256",
    Buffer.from(`${h}.${c}`),
    alg === "ES256" ? { key, dsaEncoding: "ieee-p1363" } : key,
    Buffer.from(s, "base64url"),
  );
  assert.ok(ok, `${alg} signature`);
  return { header, claims: b64urlJSON(c) };
}

async function post(path: string, body: unknown): Promise<{ status: number; json: Record<string, unknown> }> {
  const res = await fetch(relayURL + path, { method: "POST", body: JSON.stringify(body) });
  return { status: res.status, json: (await res.json()) as Record<string, unknown> };
}

async function sealAPNs(token = randomBytes(32).toString("hex"), server = SERVER) {
  const r = await post("/v1/seal", { server, platform: "apns", token, environment: "production", topic: TOPIC });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  return r.json.handle as string;
}

before(async () => {
  mock = createServer((req, res) => {
    let body = "";
    req.on("data", (d) => (body += d));
    req.on("end", () => {
      seen.push({ path: req.url ?? "", headers: req.headers, body });
      if (req.url === "/token") {
        const assertion = new URLSearchParams(body).get("assertion") ?? "";
        const { claims } = checkJWT(assertion, googleKeys.publicKey, "RS256");
        assert.equal(claims.iss, "relay@test.iam.gserviceaccount.com");
        res.writeHead(200, { "content-type": "application/json" });
        res.end(JSON.stringify({ access_token: "google-access", expires_in: 3600 }));
      } else if (req.url?.startsWith("/fcm/")) {
        const ok = req.headers.authorization === "Bearer google-access";
        res.writeHead(ok ? 200 : 401, { "content-type": "application/json" });
        res.end(ok ? `{"name":"projects/p/messages/1"}` : `{"error":{"status":"UNAUTHENTICATED"}}`);
      } else if (req.url?.startsWith("/3/device/")) {
        const jwt = (req.headers.authorization ?? "").replace(/^bearer /, "");
        const { header, claims } = checkJWT(jwt, apnsKeys.publicKey, "ES256");
        assert.equal(header.kid, "KEYID12345");
        assert.equal(claims.iss, "TEAMID1234");
        const gone = req.url.endsWith(GONE_TOKEN);
        res.writeHead(gone ? 410 : 200, gone ? { "content-type": "application/json" } : {});
        res.end(gone ? `{"reason":"Unregistered"}` : "");
      } else {
        res.writeHead(404).end();
      }
    });
  });
  await new Promise<void>((r) => mock.listen(0, "127.0.0.1", r));
  mockURL = `http://127.0.0.1:${(mock.address() as { port: number }).port}`;

  dir = mkdtempSync(join(tmpdir(), "relay-test-"));
  const pem = (k: KeyObject) => k.export({ format: "pem", type: "pkcs8" }).toString();
  const account = JSON.stringify({
    project_id: "p",
    client_email: "relay@test.iam.gserviceaccount.com",
    private_key: pem(googleKeys.privateKey),
  });
  writeFileSync(
    join(dir, ".env"),
    [
      `SEAL_KEYS=k2:${randomBytes(32).toString("base64")}`,
      `APNS_KEY="${pem(apnsKeys.privateKey).replace(/\n/g, "\\n")}"`,
      "APNS_KEY_ID=KEYID12345",
      "APNS_TEAM_ID=TEAMID1234",
      `FCM_CREDENTIALS='${account}'`,
      `APNS_HOST=${mockURL}`,
      `FCM_TOKEN_URL=${mockURL}/token`,
      `FCM_SEND_URL=${mockURL}/fcm/%s`,
      "BLOCKED_SERVERS=blocked.example",
    ].join("\n") + "\n",
  );
  const port = 20000 + Math.floor(Math.random() * 20000);
  relayURL = `http://127.0.0.1:${port}`;
  relay = spawn(
    "npx",
    ["wrangler", "dev", "-c", "relay/wrangler.jsonc", "--port", String(port), "--ip", "127.0.0.1",
      "--env-file", join(dir, ".env"), "--persist-to", join(dir, "state")],
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
    relay.stdout!.on("data", onData);
    relay.stderr!.on("data", onData);
  });
});

after(() => {
  relay?.kill();
  mock?.close();
  if (dir) rmSync(dir, { recursive: true, force: true });
});

test("info", async () => {
  const res = await fetch(relayURL + "/v1");
  assert.deepEqual(await res.json(), { relay: "compass-push", version: 1, apns: true, fcm: true });
});

test("a sent tab's alert to an iPhone", async () => {
  const token = randomBytes(32).toString("hex");
  const handle = await sealAPNs(token);
  assert.ok(!handle.includes(token), "the handle doesn't show the token");
  seen.length = 0;
  const r = await post("/v1/push", { server: SERVER, handle, type: "send-tab" });
  assert.deepEqual(r, { status: 200, json: { result: "sent" } });
  assert.equal(seen.length, 1);
  assert.equal(seen[0].path, `/3/device/${token}`);
  assert.equal(seen[0].headers["apns-topic"], TOPIC);
  assert.equal(seen[0].headers["apns-push-type"], "alert");
  assert.equal(seen[0].headers["apns-priority"], "10");
  assert.equal(JSON.parse(seen[0].body).compass.type, "send-tab");
});

test("a background sync, then the hourly limit", async () => {
  const handle = await sealAPNs();
  seen.length = 0;
  for (let i = 0; i < 6; i++) {
    assert.equal((await post("/v1/push", { server: SERVER, handle, type: "sync" })).status, 200);
  }
  assert.equal(seen[0].headers["apns-push-type"], "background");
  assert.equal(seen[0].headers["apns-priority"], "5");
  assert.deepEqual(JSON.parse(seen[0].body), { aps: { "content-available": 1 }, compass: { type: "sync" } });
  const limited = await post("/v1/push", { server: SERVER, handle, type: "sync" });
  assert.deepEqual(limited, { status: 429, json: { error: "rate_limited" } });
  assert.equal(seen.length, 6);
  // Sent tabs have their own count.
  assert.equal((await post("/v1/push", { server: SERVER, handle, type: "send-tab" })).status, 200);
});

test("a gone device", async () => {
  const handle = await sealAPNs(GONE_TOKEN);
  seen.length = 0;
  assert.deepEqual(await post("/v1/push", { server: SERVER, handle, type: "send-tab" }), {
    status: 410,
    json: { error: "gone" },
  });
  // Not sent to again.
  assert.equal((await post("/v1/push", { server: SERVER, handle, type: "send-tab" })).status, 410);
  assert.equal(seen.length, 1);
});

test("handles only work for their server, and can't be forged", async () => {
  const handle = await sealAPNs();
  const other = await post("/v1/push", { server: "other.example", handle, type: "send-tab" });
  assert.deepEqual(other, { status: 400, json: { error: "bad_handle" } });
  const [id, body] = handle.split(".");
  const bytes = Buffer.from(body, "base64url");
  bytes[bytes.length - 1] ^= 1;
  for (const forged of [`${id}.${bytes.toString("base64url")}`, `k9.${body}`, "nonsense", ""]) {
    const r = await post("/v1/push", { server: SERVER, handle: forged, type: "send-tab" });
    assert.equal(r.status, 400, forged);
  }
});

test("only Compass's pushes, apps and well-formed tokens", async () => {
  const handle = await sealAPNs();
  for (const type of ["alert", "", undefined, "custom"]) {
    assert.equal((await post("/v1/push", { server: SERVER, handle, type })).status, 400);
  }
  const base = { server: SERVER, platform: "apns", token: randomBytes(32).toString("hex"), environment: "production", topic: TOPIC };
  for (const bad of [
    { topic: "com.example.other" },
    { token: "xyz" },
    { environment: "staging" },
    { platform: "webpush" },
    { server: "Not A Host!" },
  ]) {
    assert.equal((await post("/v1/seal", { ...base, ...bad })).status, 400, JSON.stringify(bad));
  }
  const blocked = await post("/v1/seal", { ...base, server: "blocked.example" });
  assert.deepEqual(blocked, { status: 403, json: { error: "server_blocked" } });
  const notJSON = await fetch(relayURL + "/v1/push", { method: "POST", body: "{" });
  assert.equal(notJSON.status, 400);
  assert.equal((await fetch(relayURL + "/v1/push")).status, 405);
});

test("Android: a sent tab through FCM, no background push", async () => {
  const token = "fcm:" + randomBytes(40).toString("base64url");
  const sealed = await post("/v1/seal", { server: SERVER, platform: "fcm", token, topic: TOPIC });
  assert.equal(sealed.status, 200);
  const handle = sealed.json.handle as string;
  seen.length = 0;
  assert.deepEqual(await post("/v1/push", { server: SERVER, handle, type: "send-tab" }), {
    status: 200,
    json: { result: "sent" },
  });
  const send = seen.find((s) => s.path === "/fcm/p");
  assert.ok(send, JSON.stringify(seen.map((s) => s.path)));
  assert.deepEqual(JSON.parse(send.body).message, {
    token,
    data: { type: "send-tab", subtype: "it.cipi1965.compass.sync-push" },
    android: { priority: "high", ttl: "86400s" },
  });
  seen.length = 0;
  assert.deepEqual(await post("/v1/push", { server: SERVER, handle, type: "sync" }), {
    status: 200,
    json: { result: "not_sent" },
  });
  assert.equal(seen.length, 0);
});
