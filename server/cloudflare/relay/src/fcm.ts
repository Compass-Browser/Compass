// FCM HTTP v1, as server/push/fcm.go: a service account's RS256 JWT is
// exchanged for an OAuth token, kept until five minutes before it expires.
// The message carries no content, only a wake-up for Compass for Android.

import { b64url, type Env, pemBody, type Registration, type Sent } from "./env";

const TOKEN_URL = "https://oauth2.googleapis.com/token";
const SEND_URL = "https://fcm.googleapis.com/v1/projects/%s/messages:send";
const SCOPE = "https://www.googleapis.com/auth/firebase.messaging";
// The Instance ID app Compass for Android registers its token for
// (CompassPushAndroid::kAppId); Chromium drops a message without it.
const SUBTYPE = "it.cipi1965.compass.sync-push";

interface Account {
  project_id: string;
  client_email: string;
  private_key: string;
}

let access: { source: string; token: string; expires: number } | undefined;

function account(env: Env): Account | null {
  try {
    const a = JSON.parse(env.FCM_CREDENTIALS ?? "") as Account;
    return a.project_id && a.client_email && a.private_key ? a : null;
  } catch {
    return null;
  }
}

export function fcmEnabled(env: Env): boolean {
  return account(env) !== null;
}

async function accessToken(env: Env, a: Account): Promise<string> {
  const now = Math.floor(Date.now() / 1000);
  if (access && access.source === env.FCM_CREDENTIALS && now < access.expires - 300) return access.token;
  const tokenURL = env.FCM_TOKEN_URL || TOKEN_URL;
  const key = await crypto.subtle.importKey(
    "pkcs8",
    pemBody(a.private_key),
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const input = `${b64url(JSON.stringify({ alg: "RS256", typ: "JWT" }))}.${b64url(
    JSON.stringify({ iss: a.client_email, scope: SCOPE, aud: tokenURL, iat: now, exp: now + 3600 }),
  )}`;
  const sig = await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, new TextEncoder().encode(input));
  const res = await fetch(tokenURL, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      grant_type: "urn:ietf:params:oauth:grant-type:jwt-bearer",
      assertion: `${input}.${b64url(sig)}`,
    }),
  });
  if (res.status !== 200) throw new Error(`FCM token: HTTP ${res.status}`);
  const body = (await res.json()) as { access_token?: string; expires_in?: number };
  if (!body.access_token) throw new Error("FCM token: no access_token");
  access = { source: env.FCM_CREDENTIALS!, token: body.access_token, expires: now + (body.expires_in ?? 3600) };
  return access.token;
}

// sendFCM wakes the device for a sent tab (FCM gets no background push, as
// on the Go server: Android syncs on its own schedule).
export async function sendFCM(env: Env, reg: Registration): Promise<Sent> {
  const a = account(env);
  if (!a) return { status: 0, reason: "no FCM account" };
  let res: Response;
  try {
    const sendURL = (env.FCM_SEND_URL || SEND_URL).replace("%s", encodeURIComponent(a.project_id));
    res = await fetch(sendURL, {
      method: "POST",
      headers: { authorization: `Bearer ${await accessToken(env, a)}`, "content-type": "application/json" },
      body: JSON.stringify({
        message: {
          token: reg.t,
          data: { type: "send-tab", subtype: SUBTYPE },
          android: { priority: "high", ttl: "86400s" },
        },
      }),
    });
  } catch (e) {
    return { status: 0, reason: String(e) };
  }
  let reason = "";
  if (res.status !== 200) {
    try {
      reason = ((await res.json()) as { error?: { status?: string } }).error?.status ?? "";
    } catch {
      // No body.
    }
    if (res.status === 401) access = undefined;
  } else {
    await res.body?.cancel();
  }
  return { status: res.status, reason };
}

// fcmGone says the registration is gone (fcm.go, FCMInvalid).
export function fcmGone(s: Sent): boolean {
  return s.status === 404 || s.reason === "UNREGISTERED" || (s.status === 400 && s.reason === "INVALID_ARGUMENT");
}

export function validFCMToken(t: string): boolean {
  return t.length >= 32 && t.length <= 4096 && /^[A-Za-z0-9:_-]+$/.test(t);
}
