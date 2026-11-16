// Reads a shared Boost from a compassbrowser.org/boost link's fragment, in
// the browser: nothing is sent anywhere (docs/design/boosts.md, "Sharing").
// The fragment is base64url (no padding) of the raw DEFLATE of the Boost's
// JSON: {"v":1,"name","match","css","js","js_world"}.

// What a link may decompress to, at most: a decompression bomb guard.
export const MAX_BYTES = 512 * 1024;

export class BoostLinkError extends Error {
  constructor(kind, message) {
    super(message);
    // 'invalid', 'newer' or 'too_big', as common/boosts/share_vectors.json.
    this.kind = kind;
  }
}

function invalid() {
  return new BoostLinkError(
      'invalid', 'This link is incomplete or isn’t a Boost.');
}

function fromBase64Url(text) {
  if (!/^[A-Za-z0-9_-]+={0,2}$/.test(text)) {
    throw invalid();
  }
  const base64 = text.replace(/=+$/, '').replace(/-/g, '+').replace(/_/g, '/');
  let binary;
  try {
    binary = atob(base64 + '='.repeat((4 - base64.length % 4) % 4));
  } catch {
    throw invalid();
  }
  return Uint8Array.from(binary, c => c.charCodeAt(0));
}

async function inflateRaw(bytes) {
  const stream =
      new Blob([bytes]).stream().pipeThrough(
          new DecompressionStream('deflate-raw'));
  const reader = stream.getReader();
  const chunks = [];
  let size = 0;
  try {
    for (;;) {
      const {done, value} = await reader.read();
      if (done) {
        break;
      }
      size += value.length;
      if (size > MAX_BYTES) {
        reader.cancel().catch(() => {});
        throw new BoostLinkError(
            'too_big', 'This Boost is too big: shared Boosts are at most ' +
                '512 KB.');
      }
      chunks.push(value);
    }
  } catch (error) {
    throw error instanceof BoostLinkError ? error : invalid();
  }
  const out = new Uint8Array(size);
  let at = 0;
  for (const chunk of chunks) {
    out.set(chunk, at);
    at += chunk.length;
  }
  return out;
}

function optionalString(object, key) {
  const value = object[key];
  if (value === undefined) {
    return '';
  }
  if (typeof value !== 'string') {
    throw invalid();
  }
  return value;
}

// The Boost in |fragment| (without "#"): {name, match, css, js, jsWorld}.
// Only reads it: Compass checks it again when it's added.
export async function decodeBoost(fragment) {
  const bytes = await inflateRaw(fromBase64Url(fragment));
  let data;
  try {
    data = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(bytes));
  } catch {
    throw invalid();
  }
  if (!data || typeof data !== 'object' || Array.isArray(data) ||
      !Number.isInteger(data.v)) {
    throw invalid();
  }
  if (data.v !== 1) {
    throw new BoostLinkError(
        'newer',
        'This Boost was made by a newer Compass. Update Compass to add it.');
  }
  if (typeof data.match !== 'string') {
    throw invalid();
  }
  const boost = {
    name: optionalString(data, 'name'),
    match: data.match,
    css: optionalString(data, 'css'),
    js: optionalString(data, 'js'),
    jsWorld: optionalString(data, 'js_world') || 'isolated',
  };
  if (!boost.css.trim() && !boost.js.trim()) {
    throw invalid();
  }
  return boost;
}
