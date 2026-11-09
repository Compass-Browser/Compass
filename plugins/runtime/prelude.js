// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Runs inside a plugin's context, after the SDK scripts and before the
// plugin. It turns the host's primitives into the `compass` object plugins
// see, and defines the entry point the host calls.
//
// Every runtime (desktop/Android Worker, iOS JSContext, tools/live_folders)
// loads, in order: sdk/text.js, sdk/xml.js, sdk/html.js, sdk/ics.js, this
// file, then the plugin's entry script. Before that it defines
// `globalThis.__compassHost`:
//
//   {
//     fetch(request) → Promise<{status, url, headers, body}>
//         request: {url, method, headers, body, credentials}
//         Rejects with an Error whose `code` says why (host_not_allowed,
//         too_many_fetches, response_too_large, network).
//     log(message),
//     env: {now, locale, timeZone, secrets: {key: value}},
//   }
//
// The host enforces every limit itself: nothing here is a security
// boundary, since the plugin shares this context.

(function (root) {
  'use strict';

  const host = root.__compassHost;
  delete root.__compassHost;
  const sdk = root.CompassSDK || {};
  const env = host.env || {};
  const secrets = Object.assign({}, env.secrets || {});

  function headerObject(h) {
    const out = {};
    if (!h) return out;
    for (const k of Object.keys(h)) out[k.toLowerCase()] = String(h[k]);
    return out;
  }

  function response(r) {
    const body = typeof r.body === 'string' ? r.body : '';
    const headers = Object.freeze(headerObject(r.headers));
    return Object.freeze({
      status: r.status,
      ok: r.status >= 200 && r.status < 300,
      url: r.url,
      headers,
      text: () => Promise.resolve(body),
      json: () => new Promise((resolve) => resolve(JSON.parse(body))),
    });
  }

  function fetch(url, options) {
    const o = options || {};
    const request = {
      url: String(url),
      method: String(o.method || 'GET').toUpperCase(),
      headers: headerObject(o.headers),
      body: o.body === undefined || o.body === null ? null : String(o.body),
      credentials: o.credentials === 'include' ? 'include' : 'omit',
    };
    return Promise.resolve(host.fetch(request)).then(response);
  }

  const compass = Object.freeze({
    api: 1,
    fetch,
    secret: (key) => (Object.prototype.hasOwnProperty.call(secrets, key) ? secrets[key] : null),
    log: (...args) => host.log(args.map((a) => {
      if (typeof a === 'string') return a;
      try {
        return JSON.stringify(a);
      } catch (e) {
        return String(a);
      }
    }).join(' ')),
    now: () => (typeof env.now === 'number' ? env.now : Date.now()),
    locale: env.locale || 'en-US',
    timeZone: env.timeZone || 'UTC',
    text: sdk.text,
    xml: sdk.xml,
    html: sdk.html,
    ics: sdk.ics,
  });

  Object.defineProperty(root, 'compass', {value: compass, enumerable: false});

  // The host's entry point. Returns whatever the plugin returned; the host
  // normalizes it outside this context (runtime/result.js).
  Object.defineProperty(root, '__compassRun', {
    value: function (config) {
      if (typeof root.fetchItems !== 'function') {
        const e = new Error('The plugin does not define fetchItems(config, compass)');
        e.code = 'bad_plugin';
        return Promise.reject(e);
      }
      return Promise.resolve().then(() => root.fetchItems(Object.freeze(Object.assign({}, config)), compass));
    },
  });
})(globalThis);
