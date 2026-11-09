// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Pulls links and a few facts out of HTML without a DOM. Plugins reach it
// as `compass.html`:
//
//   compass.html.links(page, pageUrl)  → [{url, text, title}]
//   compass.html.title(page)           → the <title>, as text
//
// It's pattern based, not a parser: enough for "the links on this page",
// not for scraping deep structure. Prefer a JSON API when a site has one.

(function (root) {
  'use strict';

  const text = (root.CompassSDK && root.CompassSDK.text) ||
      (typeof require === 'function' ? require('./text.js') : null);

  function clean(html) {
    return String(html == null ? '' : html)
        .replace(/<!--[\s\S]*?-->/g, '')
        .replace(/<(script|style|noscript|template|svg)\b[\s\S]*?<\/\1\s*>/gi, '');
  }

  function resolve(href, base) {
    try {
      return new URL(href, base || undefined).href;
    } catch (e) {
      return null;
    }
  }

  function baseOf(html, pageUrl) {
    const m = /<base\b([^>]*)>/i.exec(html);
    if (m) {
      const href = text.parseAttributes(m[1], true).href;
      if (href) return resolve(href, pageUrl) || pageUrl;
    }
    return pageUrl;
  }

  // Every http(s) link with its visible text (or, failing that, its title,
  // aria-label or an image's alt). Fragment-only and javascript: links are
  // skipped. Fragments are dropped from the URL. Duplicate URLs keep the
  // first occurrence that has text.
  function links(html, pageUrl) {
    const src = clean(html);
    const base = baseOf(src, pageUrl);
    const out = [];
    const seen = new Map();
    const re = /<a\b([^>]*)>([\s\S]*?)<\/a\s*>/gi;
    let m;
    while ((m = re.exec(src)) !== null) {
      const attrs = text.parseAttributes(m[1], true);
      if (!attrs.href || /^\s*(#|javascript:|mailto:|tel:)/i.test(attrs.href)) continue;
      const url = resolve(attrs.href.trim(), base);
      if (!url || !/^https?:/i.test(url)) continue;
      const bare = url.replace(/#.*$/, '');
      let label = text.stripTags(m[2]);
      if (!label) {
        const img = /<img\b([^>]*)>/i.exec(m[2]);
        const alt = img ? text.parseAttributes(img[1], true).alt : '';
        label = text.collapse(attrs.title || attrs['aria-label'] || alt || '');
      }
      const link = {url: bare, text: label, title: text.collapse(attrs.title || '')};
      if (seen.has(bare)) {
        const prev = seen.get(bare);
        if (!prev.text && label) prev.text = label;
        continue;
      }
      seen.set(bare, link);
      out.push(link);
    }
    return out;
  }

  function title(html) {
    const m = /<title\b[^>]*>([\s\S]*?)<\/title\s*>/i.exec(clean(html));
    return m ? text.stripTags(m[1]) : '';
  }

  const api = Object.freeze({links, title});
  root.CompassSDK = root.CompassSDK || {};
  root.CompassSDK.html = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
