// Push through the Compass push relay (relay/README.md): this server has
// no APNs or FCM keys, so each device's push token is sealed by the relay
// into a handle, stored in the chain's object instead of the token, and
// pushes are asked of the relay. The rules are server/push/push.go's: a new
// sent tab alerts the chain's other devices; a change to spaces, passwords
// or extensions has its other iPhones sync in the background, at most once
// per 15 minutes each (COMPASS_SYNC_PUSH_BACKGROUND_SECONDS) and not while
// Compass is open there (its change stream is connected).

import type { Config } from "./config";

// SEND_TAB_TO_SELF's EntitySpecifics field number.
export const SEND_TAB_TYPE = 601980;
// The types whose changes wake iPhones in the background (push.go,
// BackgroundTypes).
export const BACKGROUND_TYPES = new Set([
  970000, // COMPASS_SPACES
  45873, // PASSWORDS
  48119, // EXTENSIONS
]);
// Registrations kept per chain (datastore.MaxPushTokensPerChain).
export const MAX_REGISTRATIONS = 20;

export interface Registration {
  token: string;
  environment: string; // "sandbox", "production"; "" for FCM
  topic: string;
  platform: "apns" | "fcm";
}

// parseRegistration checks a POST /v2/compass/push body as push.go does;
// the error is the Go server's message.
export function parseRegistration(raw: string, cfg: Config): Registration | string {
  let body: Record<string, unknown>;
  try {
    body = JSON.parse(raw);
    if (!body || typeof body !== "object") throw new Error();
  } catch {
    return "JSON body required";
  }
  const platform = body.platform ?? "apns";
  const topic = typeof body.topic === "string" ? body.topic : "";
  let token = typeof body.token === "string" ? body.token : "";
  if (platform === "fcm") {
    if (!(token.length >= 32 && token.length <= 4096 && /^[A-Za-z0-9:_-]+$/.test(token))) {
      return "token: FCM registration token required";
    }
    if (!cfg.pushTopics.includes(topic)) return "topic: not an app this server pushes to";
    return { token, environment: "", topic, platform: "fcm" };
  }
  if (platform !== "apns") return `platform: "apns" or "fcm"`;
  token = token.toLowerCase();
  if (!(token.length >= 32 && token.length <= 200 && token.length % 2 === 0 && /^[0-9a-f]+$/.test(token))) {
    return "token: hex device token required";
  }
  if (body.environment !== "sandbox" && body.environment !== "production") {
    return `environment: "sandbox" or "production"`;
  }
  if (!cfg.pushTopics.includes(topic)) return "topic: not an app this server pushes to";
  return { token, environment: body.environment, topic, platform: "apns" };
}

export async function tokenHash(platform: string, token: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${platform}:${token}`));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

const TIMEOUT_MS = 15_000;

// seal has the relay seal a registration for |server| (this server's host):
// the handle, "refused" if the relay won't take it, or null if it couldn't
// be reached.
export async function seal(relay: string, server: string, reg: Registration): Promise<string | "refused" | null> {
  try {
    const res = await fetch(`${relay}/v1/seal`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ server, ...reg }),
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });
    if (res.status === 400 || res.status === 403) {
      await res.body?.cancel();
      return "refused";
    }
    if (res.status !== 200) {
      await res.body?.cancel();
      return null;
    }
    const { handle } = (await res.json()) as { handle?: string };
    return typeof handle === "string" && handle ? handle : null;
  } catch {
    return null;
  }
}

// The relay's answer to a push: "sent", "drop" (the handle is no good or
// the device is gone: forget it), or "failed" (try at the next change).
export type PushResult = "sent" | "drop" | "failed";

export async function relayPush(
  relay: string,
  server: string,
  handle: string,
  type: "send-tab" | "sync",
): Promise<PushResult> {
  try {
    const res = await fetch(`${relay}/v1/push`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ server, handle, type }),
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });
    let error = "";
    try {
      error = ((await res.json()) as { error?: string }).error ?? "";
    } catch {
      // No JSON.
    }
    if (res.status === 200) return "sent";
    if (res.status === 410 || error === "bad_handle") return "drop";
    console.warn(`push relay: ${type} refused, ${res.status} ${error}`);
    return "failed";
  } catch (e) {
    console.warn(`push relay: ${type} failed, ${String(e)}`);
    return "failed";
  }
}
