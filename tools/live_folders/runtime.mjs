// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A reference live folder runtime on Node: runs one plugin in a fresh vm
// context with the same scripts, host primitives and limits as the browser
// runtimes (docs/design/live-folders.md, "Runtime"). Used by run.mjs for
// fixture tests and for trying a plugin against the real network.

import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';

const require = createRequire(import.meta.url);
export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const PLUGINS = path.join(ROOT, 'plugins');

const manifestLib = require(path.join(PLUGINS, 'runtime/manifest.js'));
const resultLib = require(path.join(PLUGINS, 'runtime/result.js'));

export const CONTEXT_SCRIPTS = [
  'sdk/text.js', 'sdk/xml.js', 'sdk/html.js', 'sdk/ics.js', 'runtime/prelude.js',
].map((p) => ({name: p, source: readFileSync(path.join(PLUGINS, p), 'utf8')}));

export const DEFAULT_LIMITS = Object.freeze({
  timeoutMs: 30000,
  maxFetches: 10,
  maxResponseBytes: 2 * 1024 * 1024,
  maxRedirects: 5,
  maxItems: resultLib.LIMITS.maxItems,
});

// Headers a plugin can't set: the host owns them.
const FORBIDDEN_HEADERS = new Set([
  'cookie', 'cookie2', 'host', 'origin', 'referer', 'content-length',
  'connection', 'keep-alive', 'transfer-encoding', 'upgrade', 'te',
  'proxy-authorization', 'set-cookie',
]);
const METHODS = new Set(['GET', 'HEAD', 'POST']);

export class RuntimeError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

export function loadPlugin(dir) {
  const raw = JSON.parse(readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
  const manifest = manifestLib.validateManifest(raw);
  const source = readFileSync(path.join(dir, manifest.entry), 'utf8');
  return {dir, manifest, source};
}

function decodeBody(bytes, contentType) {
  const m = /charset\s*=\s*"?([^";\s]+)/i.exec(contentType || '');
  try {
    return new TextDecoder(m ? m[1] : 'utf-8').decode(bytes);
  } catch {
    return new TextDecoder('utf-8').decode(bytes);
  }
}

// Fetches over the network, checking the allowlist on every redirect hop.
// Node has no browser cookies, so `withCookies` can only be reported.
export function networkFetcher({log}) {
  return async (req, {allowed, limits}) => {
    if (req.withCookies) log('(harness: no browser cookies; request sent without them)');
    let url = req.url;
    for (let hop = 0; ; hop++) {
      const res = await fetch(url, {
        method: req.method, headers: req.headers, body: req.body ?? undefined,
        redirect: 'manual',
      });
      if (res.status >= 300 && res.status < 400 && res.headers.get('location')) {
        if (hop >= limits.maxRedirects) throw new RuntimeError('network', 'Too many redirects');
        url = new URL(res.headers.get('location'), url).href;
        if (!manifestLib.urlAllowed(url, allowed.hosts)) {
          throw new RuntimeError('host_not_allowed', `Redirect to ${new URL(url).hostname} is not allowed`);
        }
        continue;
      }
      const declared = Number(res.headers.get('content-length') || 0);
      if (declared > limits.maxResponseBytes) throw new RuntimeError('response_too_large', `Response from ${url} is too large`);
      const bytes = new Uint8Array(await res.arrayBuffer());
      const headers = {};
      res.headers.forEach((v, k) => { headers[k] = v; });
      return {status: res.status, url, headers, bytes};
    }
  };
}

// Serves recorded responses: {"<url>" | "POST <url>": {status, headers,
// file | body | json, rejectHeaders, groups}}, files relative to `dir`.
// `groups` is what the host's HTML parse gives for a preview's `select`.
// rejectHeaders fails the request if it carries one of those headers.
export function fixtureFetcher(responses, dir) {
  return async (req) => {
    const entry = responses[`${req.method} ${req.url}`] ?? (req.method === 'GET' ? responses[req.url] : undefined);
    if (!entry) throw new RuntimeError('network', `No fixture for ${req.method} ${req.url}`);
    for (const h of entry.rejectHeaders ?? []) {
      if (Object.keys(req.headers).some((k) => k.toLowerCase() === h)) {
        throw new RuntimeError('network', `Fixture ${req.url} received header ${h}`);
      }
    }
    let bytes;
    if (entry.file) bytes = new Uint8Array(readFileSync(path.join(dir, entry.file)));
    else if (entry.json !== undefined) bytes = new TextEncoder().encode(JSON.stringify(entry.json));
    else bytes = new TextEncoder().encode(entry.body ?? '');
    return {status: entry.status ?? 200, url: req.url, headers: entry.headers ?? {}, bytes, groups: entry.groups};
  };
}

// Tab previews (docs/design/live-folders.md, "Tab previews"): no network,
// unless the user let the plugin use the tab's session.
export const PREVIEW_LIMITS = Object.freeze({
  timeoutMs: 2000,
  maxFetches: 0,
  sessionTimeoutMs: 5000,
  sessionMaxFetches: 5,
  maxResponseBytes: 2 * 1024 * 1024,
  maxRedirects: 5,
  maxSnapshotBytes: 256 * 1024,
  maxPageText: 50000,
});

function hostOf(url) {
  try {
    const u = new URL(url);
    if (u.protocol !== 'http:' && u.protocol !== 'https:') return null;
    if (u.username || u.password) return null;
    return u.hostname.toLowerCase();
  } catch {
    return null;
  }
}

// Where a session request may go: 'own' (the tab's site, narrowed by
// session.hosts; sent with cookies), 'related' (session.related; never
// cookies) or null (refused).
export function classifyPreviewUrl(manifest, tabUrl, url) {
  const session = manifest.preview && manifest.preview.session;
  const host = hostOf(url);
  const tabHost = hostOf(tabUrl);
  if (!session || !host || !tabHost) return null;
  const site = manifestLib.siteOf(tabHost);
  if (site && manifestLib.siteOf(host) === site) {
    if (session.hosts.length && !session.hosts.some((p) => manifestLib.hostMatches(p, host))) return null;
    return 'own';
  }
  return session.related.some((p) => manifestLib.hostMatches(p, host)) ? 'related' : null;
}

// Whether a preview item's icon may load: the tab's site, and with the
// session granted, the related hosts the user saw when granting it.
export function previewIconAllowed(manifest, tabUrl, granted, url) {
  const host = hostOf(url);
  const tabHost = hostOf(tabUrl);
  if (!host || !tabHost) return false;
  const site = manifestLib.siteOf(tabHost);
  if (site && manifestLib.siteOf(host) === site) return true;
  const session = manifest.preview && manifest.preview.session;
  return !!(granted && session && session.related.some((p) => manifestLib.hostMatches(p, host)));
}

// The snapshot a host hands previewTab(): only the manifest's groups, capped,
// and cookies only with the session granted. Browser hosts take it with the
// page agent (common/claude/page_agent.js, previewSnapshot); fixture cases
// give it directly.
export function normalizeSnapshot(manifest, raw, granted) {
  const preview = manifest.preview;
  const page = {
    url: String(raw.url ?? ''),
    title: String(raw.title ?? ''),
    live: raw.live !== false,
    groups: {},
  };
  for (const [name, group] of Object.entries(preview.select)) {
    const nodes = page.live && Array.isArray(raw.groups?.[name]) ? raw.groups[name] : [];
    page.groups[name] = nodes.slice(0, group.max);
  }
  if (preview.pageText && page.live && typeof raw.text === 'string') {
    page.text = raw.text.slice(0, PREVIEW_LIMITS.maxPageText);
  }
  if (granted && preview.session && preview.session.pageCookies.length && page.live) {
    page.cookies = Object.fromEntries(Object.entries(raw.cookies ?? {})
        .filter(([k]) => preview.session.pageCookies.includes(k)));
  }
  if (JSON.stringify(page).length > PREVIEW_LIMITS.maxSnapshotBytes) {
    throw new RuntimeError('response_too_large', 'The page snapshot is too large');
  }
  return page;
}

// Runs `plugin`'s previewTab() once on a snapshot. Never throws; returns the
// same shape as runPlugin(). Fixture responses for session requests may give
// `groups` (the selector group already applied, since parsing HTML is the
// host's job) and redirects as {status: 302, headers: {location}}.
export async function runPreview(plugin, {page: rawPage, config, grantSession = false,
                                          env = {}, limits = {}, fetcher}) {
  const lim = {...PREVIEW_LIMITS, ...limits};
  const log = [];
  const fetches = [];
  let timer;
  try {
    const preview = plugin.manifest.preview;
    if (!preview) throw new RuntimeError('bad_plugin', 'The plugin has no preview');
    const granted = !!(grantSession && preview.session);
    const cfg = manifestLib.validateConfig(plugin.manifest, config, preview.config);
    const page = normalizeSnapshot(plugin.manifest, rawPage ?? {}, granted);
    const maxFetches = granted ? lim.sessionMaxFetches : lim.maxFetches;
    let requests = 0;
    const timeoutMs = granted ? lim.sessionTimeoutMs : lim.timeoutMs;
    const fetchImpl = fetcher ?? (async () => {
      throw new RuntimeError('network', 'The Node harness only runs previews against fixtures');
    });

    const hostFetch = async (req) => {
      if (!preview.session) throw new RuntimeError('host_not_allowed', 'Previews can\'t make requests');
      if (!granted) throw new RuntimeError('session_not_granted', 'The user hasn\'t let this plugin use the tab\'s session');
      if (!METHODS.has(req.method)) throw new RuntimeError('bad_request', `Method ${req.method} is not allowed`);
      const names = req.select ?? [];
      const unknown = names.find((name) => !preview.select[name]);
      if (unknown !== undefined) {
        throw new RuntimeError('bad_request', `No preview.select group named ${unknown}`);
      }
      const headers = {};
      for (const [k, v] of Object.entries(req.headers || {})) {
        if (!FORBIDDEN_HEADERS.has(k.toLowerCase()) && !k.toLowerCase().startsWith('sec-')) headers[k] = v;
      }
      if (requests >= maxFetches) {
        throw new RuntimeError('too_many_fetches', `More than ${maxFetches} requests in one preview`);
      }
      requests++;
      // What the browser does (PreviewSessionFetcher): the tab's own site
      // is asked from the tab while it's live, with its origin and cookies;
      // a redirect there comes back opaque, so the browser sends the request
      // again and follows the hops itself, checking each. Hops on the own
      // site keep the cookies; the first hop to a related host is sent again
      // without them, and nothing after it gets them back. Only those
      // re-sends are recorded, as the browser only knows of those.
      let url = req.url;
      let method = req.method;
      let body = req.body;
      const kind = classifyPreviewUrl(plugin.manifest, page.url, url);
      if (!kind) {
        throw new RuntimeError('host_not_allowed', `${hostOf(url) ?? url} is not the tab's site or a related host`);
      }
      let cookies = kind === 'own';
      const via = cookies && page.live && req.via !== 'browser' ? 'page' : 'browser';
      fetches.push({method, url, cookies, via});
      const ask = () => fetchImpl({method, url, headers, body, withCookies: cookies, select: req.select},
                                  {limits: lim});
      const locationOf = (r) => (r.status >= 300 && r.status < 400 &&
          Object.entries(r.headers).find(([k]) => k.toLowerCase() === 'location')?.[1]) || null;
      let r = await ask();
      if (via === 'page' && locationOf(r)) {
        fetches.push({method, url, cookies, via: 'browser', redirect: true});
        r = await ask();
      }
      for (let hop = 0; locationOf(r); hop++) {
        if (hop >= lim.maxRedirects) throw new RuntimeError('network', 'Too many redirects');
        const next = new URL(locationOf(r), url).href;
        const nextKind = classifyPreviewUrl(plugin.manifest, page.url, next);
        if (!nextKind) {
          throw new RuntimeError('host_not_allowed', `Redirect to ${hostOf(next) ?? next} is not allowed`);
        }
        if (r.status === 303 || ((r.status === 301 || r.status === 302) && method === 'POST')) {
          method = 'GET';
          body = null;
        }
        url = next;
        if (cookies && nextKind !== 'own') {
          cookies = false;
          fetches.push({method, url, cookies, via: 'browser', redirect: true});
        }
        r = await ask();
      }
      if (r.bytes.length > lim.maxResponseBytes) {
        throw new RuntimeError('response_too_large', `Response from ${hostOf(url)} is over ${lim.maxResponseBytes} bytes`);
      }
      const contentType = Object.entries(r.headers).find(([k]) => k.toLowerCase() === 'content-type')?.[1];
      const out = {status: r.status, url, headers: r.headers, body: decodeBody(r.bytes, contentType)};
      if (names.length) {
        out.groups = Object.fromEntries(names.map(
            (name) => [name, (r.groups?.[name] ?? []).slice(0, preview.select[name].max)]));
      }
      return out;
    };

    const context = vm.createContext({
      URL, URLSearchParams, TextDecoder, TextEncoder, setTimeout, clearTimeout,
    }, {codeGeneration: {strings: false, wasm: false}});
    context.__compassHost = {
      fetch: hostFetch,
      log: (m) => log.push(String(m)),
      env: {
        now: typeof env.now === 'string' ? Date.parse(env.now) : (env.now ?? Date.now()),
        locale: env.locale ?? 'en-US',
        timeZone: env.timeZone ?? Intl.DateTimeFormat().resolvedOptions().timeZone,
        secrets: {},
        session: granted,
      },
    };
    const runOptions = {timeout: timeoutMs};
    for (const s of CONTEXT_SCRIPTS) vm.runInContext(s.source, context, {...runOptions, filename: s.name});
    vm.runInContext(plugin.source, context, {...runOptions, filename: plugin.manifest.entry});

    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new RuntimeError('timeout', `No preview after ${timeoutMs} ms`)), timeoutMs);
    });
    const raw = await Promise.race([context.__compassRunPreview(page, cfg), timeout]);
    const cloned = JSON.parse(JSON.stringify(raw ?? null));
    const result = resultLib.normalizeResult(cloned, {}, {
      preview: true,
      iconAllowed: (url) => previewIconAllowed(plugin.manifest, page.url, granted, url),
    });
    return {ok: true, result, fetches, log};
  } catch (e) {
    return {ok: false, error: errorOf(e), fetches, log};
  } finally {
    clearTimeout(timer);
  }
}

function errorOf(e) {
  const code = e && typeof e.code === 'string' ? e.code : 'plugin_error';
  const message = e && typeof e.message === 'string' ? e.message : String(e);
  return {code, message};
}

// Runs `plugin` (from loadPlugin) once. Never throws; returns
// {ok: true, result, fetches, log} or {ok: false, error: {code, message},
// fetches, log}.
export async function runPlugin(plugin, {config, secrets = {}, grantCookies = false,
                                         env = {}, limits = {}, fetcher}) {
  const lim = {...DEFAULT_LIMITS, ...limits};
  const log = [];
  const fetches = [];
  let timer;
  try {
    const cfg = manifestLib.validateConfig(plugin.manifest, config);
    const declared = new Set(plugin.manifest.secrets.map((s) => s.key));
    const ownSecrets = Object.fromEntries(Object.entries(secrets).filter(([k]) => declared.has(k)));
    for (const s of plugin.manifest.secrets) {
      if (!s.optional && !ownSecrets[s.key]) throw new RuntimeError('missing_secret', `${s.label} is not set`);
    }
    const allowed = manifestLib.resolveHosts(plugin.manifest, cfg, ownSecrets);
    const fetchImpl = fetcher ?? networkFetcher({log: (m) => log.push(m)});

    const hostFetch = async (req) => {
      if (fetches.length >= lim.maxFetches) {
        throw new RuntimeError('too_many_fetches', `More than ${lim.maxFetches} fetches in one refresh`);
      }
      let u;
      try {
        u = new URL(req.url);
      } catch {
        throw new RuntimeError('bad_request', `Not a URL: ${req.url}`);
      }
      if (!manifestLib.urlAllowed(u.href, allowed.hosts)) {
        throw new RuntimeError('host_not_allowed', `${u.hostname} is not in the plugin's hosts`);
      }
      if (!METHODS.has(req.method)) throw new RuntimeError('bad_request', `Method ${req.method} is not allowed`);
      const headers = {};
      for (const [k, v] of Object.entries(req.headers || {})) {
        if (!FORBIDDEN_HEADERS.has(k.toLowerCase()) && !k.toLowerCase().startsWith('sec-')) headers[k] = v;
      }
      const withCookies = req.credentials === 'include' && grantCookies &&
          allowed.cookies.some((p) => manifestLib.hostMatches(p, u.hostname));
      fetches.push({method: req.method, url: u.href, cookies: withCookies});
      const r = await fetchImpl({method: req.method, url: u.href, headers, body: req.body, withCookies},
                                {allowed, limits: lim});
      if (r.bytes.length > lim.maxResponseBytes) {
        throw new RuntimeError('response_too_large', `Response from ${u.hostname} is over ${lim.maxResponseBytes} bytes`);
      }
      const contentType = Object.entries(r.headers).find(([k]) => k.toLowerCase() === 'content-type')?.[1];
      return {status: r.status, url: r.url, headers: r.headers, body: decodeBody(r.bytes, contentType)};
    };

    const context = vm.createContext({
      URL, URLSearchParams, TextDecoder, TextEncoder, setTimeout, clearTimeout,
    }, {codeGeneration: {strings: false, wasm: false}});
    context.__compassHost = {
      fetch: hostFetch,
      log: (m) => log.push(String(m)),
      env: {
        now: typeof env.now === 'string' ? Date.parse(env.now) : (env.now ?? Date.now()),
        locale: env.locale ?? 'en-US',
        timeZone: env.timeZone ?? Intl.DateTimeFormat().resolvedOptions().timeZone,
        secrets: ownSecrets,
      },
    };
    const runOptions = {timeout: lim.timeoutMs};
    for (const s of CONTEXT_SCRIPTS) vm.runInContext(s.source, context, {...runOptions, filename: s.name});
    vm.runInContext(plugin.source, context, {...runOptions, filename: plugin.manifest.entry});

    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new RuntimeError('timeout', `No result after ${lim.timeoutMs} ms`)), lim.timeoutMs);
    });
    const raw = await Promise.race([context.__compassRun(cfg), timeout]);
    // Crossing out of the plugin's realm the way postMessage would.
    const cloned = JSON.parse(JSON.stringify(raw ?? null));
    const result = resultLib.normalizeResult(cloned, {maxItems: lim.maxItems});
    return {ok: true, result, fetches, log};
  } catch (e) {
    return {ok: false, error: errorOf(e), fetches, log};
  } finally {
    clearTimeout(timer);
  }
}
