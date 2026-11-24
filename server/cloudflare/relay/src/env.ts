// The relay's bindings (wrangler.jsonc).

import type { Device } from "./device";

export interface Env {
  DEVICE: DurableObjectNamespace<Device>;
  SERVER_LIMIT?: RateLimit;
  TOPICS?: string;
  BLOCKED_SERVERS?: string;
  SEAL_KEYS?: string;
  APNS_KEY?: string;
  APNS_KEY_ID?: string;
  APNS_TEAM_ID?: string;
  FCM_CREDENTIALS?: string;
  // Tests: replace Apple's and Google's endpoints.
  APNS_HOST?: string;
  FCM_TOKEN_URL?: string;
  FCM_SEND_URL?: string;
}

// The two pushes the relay sends; nothing else can be sent through it.
export type PushType = "send-tab" | "sync";

// What a handle seals: one device's registration, for one sync server.
export interface Registration {
  p: "apns" | "fcm"; // platform
  t: string; // the device token
  e: string; // APNs environment ("sandbox" or "production"); "" for FCM
  o: string; // topic: the app's bundle ID or package name
  h: string; // the sync server's host
  i: number; // sealed at (Unix seconds)
}

// An answer from Apple or Google.
export interface Sent {
  status: number; // their HTTP status; 0 if they couldn't be reached
  reason: string;
}

export function list(v: string | undefined): string[] {
  return (v ?? "").split(",").map((s) => s.trim().toLowerCase()).filter(Boolean);
}

export function b64url(data: ArrayBuffer | Uint8Array | string): string {
  const bytes = typeof data === "string" ? new TextEncoder().encode(data) : new Uint8Array(data);
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function unb64(s: string): Uint8Array | null {
  try {
    const bin = atob(s.replace(/-/g, "+").replace(/_/g, "/"));
    return Uint8Array.from(bin, (c) => c.charCodeAt(0));
  } catch {
    return null;
  }
}

// pemBody is a PEM's DER bytes.
export function pemBody(pem: string): Uint8Array<ArrayBuffer> {
  const der = unb64(pem.replace(/-----[^-]+-----/g, "").replace(/\s+/g, "")) ?? new Uint8Array();
  return new Uint8Array(der);
}
