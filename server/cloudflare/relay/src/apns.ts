// APNs, as server/push/apns.go: token-based auth (an ES256 JWT, reused for
// 40 minutes; Apple refuses one older than an hour). A Worker's fetch
// speaks HTTP/2 to Apple, as it requires.

import { b64url, type Env, pemBody, type PushType, type Registration, type Sent } from "./env";

const PRODUCTION = "https://api.push.apple.com";
const SANDBOX = "https://api.sandbox.push.apple.com";

// The alert a sent tab wakes an iPhone with; its notification service
// extension rewrites it ("Tab from MacBook", the page's title and host).
const SEND_TAB = `{"aps":{"alert":{"title":"New tab","body":"Open Compass to see it"},"mutable-content":1,"sound":"default","thread-id":"sent-tab"},"compass":{"type":"send-tab"}}`;
// The silent push that has an iPhone sync in the background.
const SYNC = `{"aps":{"content-available":1},"compass":{"type":"sync"}}`;

let jwt: { source: string; token: string; at: number } | undefined;

export function apnsEnabled(env: Env): boolean {
  return !!(env.APNS_KEY && env.APNS_KEY_ID && env.APNS_TEAM_ID);
}

async function providerToken(env: Env): Promise<string> {
  const now = Math.floor(Date.now() / 1000);
  const source = `${env.APNS_KEY_ID}/${env.APNS_TEAM_ID}/${env.APNS_KEY}`;
  if (jwt && jwt.source === source && now - jwt.at < 40 * 60) return jwt.token;
  const key = await crypto.subtle.importKey(
    "pkcs8",
    pemBody(env.APNS_KEY!),
    { name: "ECDSA", namedCurve: "P-256" },
    false,
    ["sign"],
  );
  const input = `${b64url(JSON.stringify({ alg: "ES256", kid: env.APNS_KEY_ID }))}.${b64url(
    JSON.stringify({ iss: env.APNS_TEAM_ID, iat: now }),
  )}`;
  // Web Crypto's ECDSA signature is the raw r||s a JWS wants.
  const sig = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, key, new TextEncoder().encode(input));
  jwt = { source, token: `${input}.${b64url(sig)}`, at: now };
  return jwt.token;
}

export async function sendAPNs(env: Env, reg: Registration, type: PushType): Promise<Sent> {
  const host = env.APNS_HOST || (reg.e === "sandbox" ? SANDBOX : PRODUCTION);
  const background = type === "sync";
  // A sent tab older than a day isn't worth a banner; a background sync an
  // hour late is near the next one anyway.
  const lifetime = background ? 3600 : 86400;
  let res: Response;
  try {
    res = await fetch(`${host}/3/device/${reg.t}`, {
      method: "POST",
      headers: {
        authorization: `bearer ${await providerToken(env)}`,
        "apns-topic": reg.o,
        "apns-push-type": background ? "background" : "alert",
        // Apple requires 5 for background pushes.
        "apns-priority": background ? "5" : "10",
        "apns-expiration": String(Math.floor(Date.now() / 1000) + lifetime),
        "content-type": "application/json",
      },
      body: background ? SYNC : SEND_TAB,
    });
  } catch (e) {
    return { status: 0, reason: String(e) };
  }
  let reason = "";
  if (res.status !== 200) {
    try {
      reason = ((await res.json()) as { reason?: string }).reason ?? "";
    } catch {
      // No body.
    }
    if (reason === "ExpiredProviderToken" || reason === "InvalidProviderToken") jwt = undefined;
  } else {
    await res.body?.cancel();
  }
  return { status: res.status, reason };
}

// apnsGone says Apple refused the token for good (apns.go, Result.Invalid).
export function apnsGone(s: Sent): boolean {
  return s.status === 410 || (s.status === 400 && (s.reason === "BadDeviceToken" || s.reason === "DeviceTokenNotForTopic"));
}

export function validAPNsToken(t: string): boolean {
  return t.length >= 32 && t.length <= 200 && t.length % 2 === 0 && /^[0-9a-f]+$/.test(t);
}
