// Sealed handles: a device's registration encrypted with a key only the
// relay holds (AES-256-GCM). The sync server keeps the handle where the Go
// server keeps the token; the relay keeps nothing. SEAL_KEYS is
// "<id>:<base64 key>,...": the first seals, all open, so a key can be
// rotated by putting a new one first and dropping the old one once devices
// have registered again (they do at every launch).

import { b64url, type Env, type Registration, unb64 } from "./env";

const AAD = new TextEncoder().encode("compass-push-relay/1");

interface Key {
  id: string;
  key: CryptoKey;
}

let loaded: { source: string; keys: Promise<Key[]> } | undefined;

function keys(env: Env): Promise<Key[]> {
  const source = env.SEAL_KEYS ?? "";
  if (loaded?.source !== source) {
    loaded = {
      source,
      keys: Promise.all(
        source
          .split(",")
          .map((s) => s.trim())
          .filter(Boolean)
          .map(async (entry) => {
            const colon = entry.indexOf(":");
            const id = entry.slice(0, colon);
            const raw = unb64(entry.slice(colon + 1));
            if (colon < 1 || !/^[a-z0-9]{1,8}$/i.test(id) || !raw || raw.length !== 32) {
              throw new Error("SEAL_KEYS: each entry is <id>:<base64 of 32 bytes>");
            }
            const key = await crypto.subtle.importKey("raw", new Uint8Array(raw), "AES-GCM", false, [
              "encrypt",
              "decrypt",
            ]);
            return { id, key };
          }),
      ),
    };
  }
  return loaded.keys;
}

export async function canSeal(env: Env): Promise<boolean> {
  try {
    return (await keys(env)).length > 0;
  } catch {
    return false;
  }
}

export async function seal(env: Env, reg: Registration): Promise<string> {
  const [current] = await keys(env);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const plain = new TextEncoder().encode(JSON.stringify(reg));
  const sealed = await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: AAD }, current.key, plain);
  const out = new Uint8Array(iv.length + sealed.byteLength);
  out.set(iv);
  out.set(new Uint8Array(sealed), iv.length);
  return `${current.id}.${b64url(out)}`;
}

// open is the handle's registration, or null if it isn't one of ours (or
// its key was dropped).
export async function open(env: Env, handle: string): Promise<Registration | null> {
  const dot = handle.indexOf(".");
  if (dot < 1 || handle.length > 8192) return null;
  const id = handle.slice(0, dot);
  const raw = unb64(handle.slice(dot + 1));
  let all: Key[];
  try {
    all = await keys(env);
  } catch {
    return null;
  }
  const k = all.find((k) => k.id === id);
  if (!k || !raw || raw.length < 12 + 16) return null;
  try {
    const plain = await crypto.subtle.decrypt(
      { name: "AES-GCM", iv: raw.subarray(0, 12), additionalData: AAD },
      k.key,
      raw.subarray(12),
    );
    const reg = JSON.parse(new TextDecoder().decode(plain)) as Registration;
    return typeof reg.t === "string" && (reg.p === "apns" || reg.p === "fcm") ? reg : null;
  } catch {
    return null;
  }
}
