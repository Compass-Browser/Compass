// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// The scripts behind iOS's developer tools (docs/design/claude.md,
/// "Developer tools", iOS). WKWebView has no DevTools protocol, so:
/// - `capture` runs in the page world from document start (main frame), as
///   ConsoleCapture does, only while the setting is on: it wraps fetch,
///   XMLHttpRequest, sendBeacon, WebSocket, EventSource and
///   addEventListener, which a separate content world can't see (its
///   globals are its own). The page can see, and lie to, these wrappers:
///   what they record is "reported by the page".
/// - `inspect`, `performance` and `indexedDBNames` only read, so they run
///   in Claude's own content world, out of the page's reach (it shares the
///   DOM, CSSOM, storage and performance timeline).
public enum DevToolsScripts {
  /// Replaced in `capture` by the reader's name, random per app run.
  public static let keyPlaceholder = "__COMPASS_DEVTOOLS_KEY__"

  /// The capture script for a reader named `key`.
  public static func capture(key: String) -> String { captureSource.replacingOccurrences(of: keyPlaceholder, with: key) }

  /// In the page world: the requests the wrappers saw, as JSON
  /// ({total, entries}), or null before the script ran.
  public static func readRequests(key: String) -> String {
    "window['\(key)'] ? window['\(key)'].requests() : null"
  }

  /// In the page world, with `selector`: the element's listeners as JSON,
  /// or null.
  public static func readListeners(key: String) -> String {
    "return window['\(key)'] ? window['\(key)'].listeners(selector) : null"
  }

  private static let captureSource = #"""
    (() => {
      const KEY = '__COMPASS_DEVTOOLS_KEY__';
      if (window[KEY]) return;
      const MAX_ENTRIES = 300, MAX_BODY = 65536, MAX_BODIES = 2 * 1024 * 1024, MAX_POST = 20000;
      const entries = [];
      let seq = 0, bodies = 0;
      const now = () => performance.now();
      const isText = (mime) => /^text\/|json|javascript|ecmascript|xml|x-www-form-urlencoded|graphql|csv|yaml/i.test(mime || '');
      const add = (entry) => {
        entry.seq = ++seq;
        entry.start = now();
        entries.push(entry);
        while (entries.length > MAX_ENTRIES) {
          const old = entries.shift();
          if (old.body) bodies -= old.body.length;
        }
        return entry;
      };
      const keepBody = (entry, text) => {
        if (typeof text !== 'string') return;
        const kept = text.slice(0, MAX_BODY);
        entry.bodyLength = text.length;
        // Oldest bodies go first when they'd take too much memory.
        for (const old of entries) {
          if (bodies + kept.length <= MAX_BODIES) break;
          if (old.body) { bodies -= old.body.length; delete old.body; old.bodyDropped = true; }
        }
        entry.body = kept;
        bodies += kept.length;
      };
      const headerList = (headers) => {
        const out = [];
        try { new Headers(headers || undefined).forEach((value, name) => out.push([name, value])); } catch (e) {}
        return out;
      };
      const postText = (body) => {
        if (body == null) return undefined;
        if (typeof body === 'string') return body.slice(0, MAX_POST);
        if (body instanceof URLSearchParams) return body.toString().slice(0, MAX_POST);
        if (typeof FormData !== 'undefined' && body instanceof FormData) {
          const parts = [];
          body.forEach((value, name) => parts.push(name + '=' + (typeof value === 'string' ? value : '[file]')));
          return parts.join('&').slice(0, MAX_POST);
        }
        return '[binary]';
      };
      const absolute = (url) => { try { return new URL(String(url), location.href).href; } catch (e) { return String(url); } };
      const message = (error) => String(error && error.message || error);

      // fetch
      const originalFetch = window.fetch;
      if (originalFetch) {
        window.fetch = function (input, init) {
          const request = (typeof Request !== 'undefined' && input instanceof Request) ? input : null;
          const entry = add({
            type: 'Fetch',
            method: String((init && init.method) || (request && request.method) || 'GET').toUpperCase(),
            url: absolute(request ? request.url : input),
            requestHeaders: headerList((init && init.headers) || (request && request.headers)),
            requestBody: postText(init && init.body),
          });
          const promise = originalFetch.apply(this, arguments);
          promise.then((response) => {
            entry.status = response.status;
            entry.statusText = response.statusText;
            entry.responseType = response.type;
            entry.responseHeaders = headerList(response.headers);
            entry.mime = response.headers.get('content-type') || '';
            const length = response.headers.get('content-length');
            if (length) entry.size = Number(length);
            if (response.type === 'opaque' || !isText(entry.mime)) {
              entry.end = now();
              return;
            }
            try {
              response.clone().text().then((text) => {
                entry.end = now();
                if (!entry.size) entry.size = text.length;
                keepBody(entry, text);
              }, (error) => { entry.end = now(); entry.bodyError = message(error); });
            } catch (error) {
              entry.end = now();
              entry.bodyError = message(error);
            }
          }, (error) => { entry.end = now(); entry.error = message(error); });
          return promise;
        };
      }

      // XMLHttpRequest
      const xhrs = new WeakMap();
      const XHR = XMLHttpRequest.prototype;
      const open = XHR.open, send = XHR.send, setHeader = XHR.setRequestHeader;
      XHR.open = function (method, url) {
        xhrs.set(this, {type: 'XHR', method: String(method || 'GET').toUpperCase(), url: absolute(url), requestHeaders: []});
        return open.apply(this, arguments);
      };
      XHR.setRequestHeader = function (name, value) {
        const pending = xhrs.get(this);
        if (pending && !pending.seq) pending.requestHeaders.push([String(name).toLowerCase(), String(value)]);
        return setHeader.apply(this, arguments);
      };
      XHR.send = function (body) {
        const pending = xhrs.get(this);
        if (pending) {
          pending.requestBody = postText(body);
          const entry = add(pending);
          const xhr = this;
          xhr.addEventListener('loadend', () => {
            entry.end = now();
            entry.status = xhr.status;
            entry.statusText = xhr.statusText;
            const raw = xhr.getAllResponseHeaders() || '';
            entry.responseHeaders = raw.trim().split(/[\r\n]+/).filter(Boolean).map((line) => {
              const colon = line.indexOf(':');
              return [line.slice(0, colon).trim().toLowerCase(), line.slice(colon + 1).trim()];
            });
            entry.mime = xhr.getResponseHeader('content-type') || '';
            if (!xhr.status && !entry.error) entry.error = 'failed (network error, CORS or blocked)';
            try {
              if (xhr.responseType === '' || xhr.responseType === 'text') {
                entry.size = xhr.responseText.length;
                if (isText(entry.mime) || !entry.mime) keepBody(entry, xhr.responseText);
              } else if (xhr.responseType === 'json') {
                keepBody(entry, JSON.stringify(xhr.response));
              }
            } catch (e) {}
          });
          for (const kind of ['abort', 'timeout']) xhr.addEventListener(kind, () => { entry.error = kind === 'abort' ? 'aborted' : 'timed out'; });
        }
        return send.apply(this, arguments);
      };

      // sendBeacon
      if (navigator.sendBeacon) {
        const beacon = navigator.sendBeacon;
        navigator.sendBeacon = function (url, data) {
          const entry = add({type: 'Ping', method: 'POST', url: absolute(url), requestBody: postText(data)});
          const queued = beacon.apply(this, arguments);
          entry.end = now();
          if (!queued) entry.error = 'not queued';
          return queued;
        };
      }

      // WebSocket and EventSource: constructors behind a Proxy, so
      // instanceof and the prototype stay theirs.
      const watch = (name, type, setUp) => {
        const Original = window[name];
        if (!Original) return;
        window[name] = new Proxy(Original, {
          construct(target, args, newTarget) {
            const socket = Reflect.construct(target, args, newTarget);
            const entry = add({type, method: 'GET', url: absolute(args[0])});
            socket.addEventListener('error', () => { entry.error = entry.error || 'failed'; });
            setUp(socket, entry);
            return socket;
          },
        });
      };
      watch('WebSocket', 'WebSocket', (socket, entry) => {
        entry.sent = 0; entry.received = 0;
        socket.addEventListener('open', () => { entry.status = 101; entry.statusText = 'Switching Protocols'; });
        socket.addEventListener('message', (event) => {
          entry.received++;
          entry.size = (entry.size || 0) + (typeof event.data === 'string' ? event.data.length : (event.data.size || event.data.byteLength || 0));
        });
        socket.addEventListener('close', (event) => { entry.end = now(); entry.closeCode = event.code; });
        const sendFrame = socket.send;
        socket.send = function () { entry.sent++; return sendFrame.apply(this, arguments); };
      });
      watch('EventSource', 'EventSource', (source, entry) => {
        entry.received = 0;
        source.addEventListener('open', () => { entry.status = 200; });
        source.addEventListener('message', () => { entry.received++; });
      });

      // Event listeners, for inspect_element: by target, while added.
      const listeners = new WeakMap();
      const ET = EventTarget.prototype;
      const addListener = ET.addEventListener, removeListener = ET.removeEventListener;
      const captureOf = (options) => typeof options === 'boolean' ? options : !!(options && options.capture);
      ET.addEventListener = function (type, handler, options) {
        try {
          if (handler && this instanceof Element) {
            let list = listeners.get(this);
            if (!list) listeners.set(this, list = []);
            if (list.length < 100) {
              list.push({
                type: String(type), handler, capture: captureOf(options),
                passive: !!(options && options.passive), once: !!(options && options.once),
              });
            }
          }
        } catch (e) {}
        return addListener.apply(this, arguments);
      };
      ET.removeEventListener = function (type, handler, options) {
        try {
          const list = listeners.get(this);
          if (list) {
            const index = list.findIndex((l) => l.type === String(type) && l.handler === handler && l.capture === captureOf(options));
            if (index >= 0) list.splice(index, 1);
          }
        } catch (e) {}
        return removeListener.apply(this, arguments);
      };

      Object.defineProperty(window, KEY, {
        enumerable: false,
        value: Object.freeze({
          requests: () => JSON.stringify({total: seq, entries}),
          listeners: (selector) => {
            const el = document.querySelector(selector);
            if (!el) return null;
            const describe = (fn) => {
              try {
                const source = typeof fn === 'function' ? Function.prototype.toString.call(fn)
                    : fn && typeof fn.handleEvent === 'function' ? 'handleEvent: ' + Function.prototype.toString.call(fn.handleEvent) : '';
                return source.replace(/\s+/g, ' ').slice(0, 150);
              } catch (e) { return ''; }
            };
            const list = (listeners.get(el) || []).map((l) => ({
              type: l.type, capture: l.capture, passive: l.passive, once: l.once, handler: describe(l.handler),
            }));
            for (const attribute of el.getAttributeNames()) {
              if (attribute.startsWith('on')) {
                list.push({type: attribute.slice(2), inline: true, handler: String(el.getAttribute(attribute)).slice(0, 150)});
              }
            }
            return JSON.stringify(list);
          },
        }),
      });
    })();
    """#

  /// In Claude's world, with `selector`, `properties` (names, or ["all"],
  /// or empty for the default list) and `userAgent`: the element's box,
  /// computed styles and matching rules as JSON.
  public static let inspect = #"""
    const el = document.querySelector(selector);
    if (!el) return JSON.stringify({error: 'No element matches ' + selector + '.'});
    const DEFAULT = ['display', 'position', 'top', 'right', 'bottom', 'left', 'z-index', 'width', 'height',
      'min-width', 'max-width', 'min-height', 'max-height', 'box-sizing', 'margin-top', 'margin-right',
      'margin-bottom', 'margin-left', 'padding-top', 'padding-right', 'padding-bottom', 'padding-left',
      'border-top-width', 'border-right-width', 'border-bottom-width', 'border-left-width', 'overflow-x',
      'overflow-y', 'flex-direction', 'flex-wrap', 'flex-grow', 'flex-shrink', 'flex-basis', 'justify-content',
      'align-items', 'align-self', 'gap', 'grid-template-columns', 'grid-template-rows', 'color',
      'background-color', 'background-image', 'font-family', 'font-size', 'font-weight', 'line-height',
      'text-align', 'white-space', 'opacity', 'visibility', 'transform', 'pointer-events', 'cursor'];
    const style = getComputedStyle(el);
    const shown = ['id', 'class', 'name', 'type', 'role', 'href', 'src', 'aria-label'];
    let node = '<' + el.localName;
    for (const name of shown) {
      if (el.hasAttribute(name)) node += ' ' + name + '="' + String(el.getAttribute(name)).slice(0, 80) + '"';
    }
    node += '>';
    const rect = el.getBoundingClientRect();
    const px = (name) => parseFloat(style.getPropertyValue(name)) || 0;
    const edges = (kind, suffix) => ['top', 'right', 'bottom', 'left'].map((side) => Math.round(px(kind + '-' + side + suffix))).join(' ');
    const rendered = style.display !== 'none' && (rect.width || rect.height || el.getClientRects().length);
    const box = rendered
      ? 'Box: ' + Math.round(rect.width) + '×' + Math.round(rect.height) + ' at ' + Math.round(rect.left + scrollX) + ',' +
        Math.round(rect.top + scrollY) + '; content ' +
        Math.round(rect.width - px('padding-left') - px('padding-right') - px('border-left-width') - px('border-right-width')) + '×' +
        Math.round(rect.height - px('padding-top') - px('padding-bottom') - px('border-top-width') - px('border-bottom-width')) +
        '; padding ' + edges('padding', '') + '; border ' + edges('border', '-width') + '; margin ' + edges('margin', '') +
        ' (top right bottom left)'
      : 'Box: none (not rendered: display: none, or not in the page)';
    const all = properties.includes('all');
    const names = all ? Array.from(style) : (properties.length ? properties : DEFAULT);
    const computed = names.map((name) => [name, style.getPropertyValue(name)]);

    // Specificity of one selector: [ids, classes, types], roughly.
    const specificity = (selector) => {
      const s = selector.replace(/\\./g, '').replace(/"[^"]*"|'[^']*'/g, '');
      const ids = (s.match(/#[\w-]+/g) || []).length;
      const classes = (s.match(/\.[\w-]+|\[[^\]]*\]|:(?!:)(?!not|is|where|has)[\w-]+/g) || []).length;
      const types = (s.replace(/:[\w-]+(\([^)]*\))?/g, '').match(/(^|[\s>+~])[a-zA-Z][\w-]*/g) || []).length +
        (s.match(/::[\w-]+/g) || []).length;
      return ids * 10000 + classes * 100 + types;
    };
    const rules = [];
    const blocked = [];
    let order = 0;
    const walk = (list, sheet, media) => {
      for (const rule of list) {
        order++;
        if (rule.selectorText !== undefined && rule.style) {
          let best = -1;
          for (const part of rule.selectorText.split(',')) {
            try { if (el.matches(part.trim())) best = Math.max(best, specificity(part.trim())); } catch (e) {}
          }
          if (best >= 0) {
            rules.push({selector: rule.selectorText, media, where: sheet, specificity: best, order,
                        declarations: Array.from(rule.style).map((name) =>
                          [name, rule.style.getPropertyValue(name), rule.style.getPropertyPriority(name)])});
          }
          if (rule.cssRules && rule.cssRules.length) walk(rule.cssRules, sheet, media);
        } else if (rule.media && rule.cssRules) {
          if (matchMedia(rule.media.mediaText).matches) walk(rule.cssRules, sheet, rule.media.mediaText);
        } else if (rule.cssRules) {
          walk(rule.cssRules, sheet, media);
        }
      }
    };
    for (const sheet of document.styleSheets) {
      const where = sheet.href || '<style> in ' + location.href;
      let list;
      try { list = sheet.cssRules; } catch (e) { blocked.push(sheet.href || '?'); continue; }
      if (sheet.media && sheet.media.length && !matchMedia(sheet.media.mediaText).matches) continue;
      walk(list, where, '');
    }
    rules.sort((a, b) => b.specificity - a.specificity || b.order - a.order);
    const inline = Array.from(el.style).map((name) => [name, el.style.getPropertyValue(name), el.style.getPropertyPriority(name)]);
    return JSON.stringify({node, box, computed, inline, rules: rules.slice(0, 21), more: rules.length > 20, blocked,
                           shadow: el.getRootNode() !== document});
    """#

  /// In Claude's world: the page's timings and vitals as JSON (the
  /// desktop's kVitalsScript, with what WebKit has).
  public static let performance = #"""
    const take = (type, extra) => new Promise((resolve) => {
      const entries = [];
      try {
        if (!PerformanceObserver.supportedEntryTypes.includes(type)) { resolve(null); return; }
        const observer = new PerformanceObserver((list) => entries.push(...list.getEntries()));
        observer.observe({type, buffered: true, ...extra});
        setTimeout(() => { observer.disconnect(); resolve(entries); }, 100);
      } catch (e) {
        resolve(null);
      }
    });
    const [lcp, shifts, events, frames] = await Promise.all([
      take('largest-contentful-paint'), take('layout-shift'),
      take('event', {durationThreshold: 16}), take('long-animation-frame')]);
    const nav = performance.getEntriesByType('navigation')[0];
    const fcp = performance.getEntriesByName('first-contentful-paint')[0];
    let cls = -1, session = 0, first = 0, last = 0;
    if (shifts) {
      cls = 0;
      for (const shift of shifts) {
        if (shift.hadRecentInput) continue;
        if (session && shift.startTime - last < 1000 && shift.startTime - first < 5000) {
          session += shift.value;
        } else {
          session = shift.value;
          first = shift.startTime;
        }
        last = shift.startTime;
        cls = Math.max(cls, session);
      }
    }
    const interactions = events ? events.filter((e) => e.interactionId) : null;
    const largest = lcp && lcp[lcp.length - 1];
    const name = (el) => el ? el.localName + (el.id ? '#' + el.id : '') : '';
    const resources = performance.getEntriesByType('resource').map((r) => ({
      url: r.name, type: r.initiatorType, duration: r.duration, size: r.transferSize || 0}));
    return JSON.stringify({
      url: location.href,
      ttfb: nav ? nav.responseStart : -1,
      fcp: fcp ? fcp.startTime : -1,
      dcl: nav ? nav.domContentLoadedEventEnd : -1,
      load: nav ? nav.loadEventEnd : -1,
      lcpSupported: lcp !== null,
      lcp: largest ? largest.startTime : -1,
      lcpElement: largest ? name(largest.element) : '',
      clsSupported: shifts !== null,
      cls,
      inpSupported: interactions !== null,
      inp: interactions ? interactions.reduce((m, e) => Math.max(m, e.duration), -1) : -1,
      interactions: interactions ? interactions.length : 0,
      framesSupported: frames !== null,
      longFrames: frames ? frames.length : 0,
      blocking: frames ? frames.reduce((t, f) => t + (f.blockingDuration || 0), 0) : 0,
      resources: resources.length,
      transfer: resources.reduce((t, r) => t + r.size, 0),
      slowest: resources.sort((a, b) => b.duration - a.duration).slice(0, 5),
      nodes: document.getElementsByTagName('*').length,
    });
    """#

  /// In Claude's world: the page's resource timings (everything it loaded,
  /// fetches included) as JSON.
  public static let resources = #"""
    const list = performance.getEntriesByType('resource').map((r) => ({
      url: r.name, type: r.initiatorType, start: r.startTime, duration: r.duration,
      size: r.transferSize || 0, encoded: r.encodedBodySize || 0,
      status: r.responseStatus || 0, protocol: r.nextHopProtocol || ''}));
    const nav = performance.getEntriesByType('navigation')[0];
    if (nav) list.unshift({url: nav.name, type: 'navigation', start: 0, duration: nav.duration,
      size: nav.transferSize || 0, encoded: nav.encodedBodySize || 0, status: nav.responseStatus || 0,
      protocol: nav.nextHopProtocol || ''});
    return JSON.stringify({origin: performance.timeOrigin, list});
    """#

  /// In Claude's world: the site's IndexedDB database names as JSON.
  public static let indexedDBNames = #"""
    if (!indexedDB.databases) return JSON.stringify({error: 'unavailable'});
    try {
      const list = await indexedDB.databases();
      return JSON.stringify({names: list.map((d) => d.name)});
    } catch (e) {
      return JSON.stringify({error: String(e && e.message || e)});
    }
    """#
}
