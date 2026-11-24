// Sync tokens (docs/design/sync.md, "Identity"): per request,
// base64url_padded(HEX(ts_ms) "|" HEX(ed25519_sign(ts_ms)) "|" HEX(pubkey)),
// where ts_ms is the ASCII decimal Unix time in ms. The chain ID is the hex
// public key; the server checks the signature and a ±24 h window
// (server/auth/auth.go).

const TOKEN_MAX_AGE_MS = 86400 * 1000;
const TOKEN_RE = /^([a-fA-F0-9]+)\|([a-fA-F0-9]+)\|([a-fA-F0-9]+)$/;

export class AuthError extends Error {}

function hexBytes(hex: string): Uint8Array {
  if (hex.length % 2) throw new AuthError("odd hex length");
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

function base64UrlDecode(s: string): string {
  const b64 = s.replace(/-/g, "+").replace(/_/g, "/");
  try {
    return atob(b64);
  } catch {
    throw new AuthError("bad base64");
  }
}

// verifyToken returns the chain ID (the public key's hex, as the token
// spells it) of a valid token.
export async function verifyToken(token: string, now = Date.now()): Promise<string> {
  const m = TOKEN_RE.exec(base64UrlDecode(token));
  if (!m) throw new AuthError("invalid token format");
  const [, tsHex, sigHex, keyHex] = m;
  const key = hexBytes(keyHex);
  const ts = hexBytes(tsHex);
  const sig = hexBytes(sigHex);
  if (key.length !== 32 || sig.length !== 64) throw new AuthError("bad key or signature length");
  let ok = false;
  try {
    const pub = await crypto.subtle.importKey("raw", key, { name: "Ed25519" }, false, ["verify"]);
    ok = await crypto.subtle.verify({ name: "Ed25519" }, pub, sig, ts);
  } catch {
    ok = false;
  }
  if (!ok) throw new AuthError("signature verification failed");
  const tsText = new TextDecoder().decode(ts);
  if (!/^-?\d+$/.test(tsText)) throw new AuthError("bad timestamp");
  if (Math.abs(now - Number(tsText)) > TOKEN_MAX_AGE_MS) throw new AuthError("token is expired");
  return keyHex;
}

// bearer returns the token of an "Authorization: Bearer <token>" header,
// or "" if there is none.
export function bearer(header: string | null): string {
  if (!header) return "";
  const t = header.trim();
  return t.startsWith("Bearer ") ? t.slice(7).trim() : "";
}

export function idList(v: string | undefined): string[] {
  return (v ?? "")
    .split(",")
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean);
}
