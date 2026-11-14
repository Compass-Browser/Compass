// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's Reader mode (docs/design/reader-mode.md), shared by desktop and
// iOS. Runs after Readability.js and Readability-readerable.js, in a world
// the page's scripts can't reach (desktop: an isolated world; iOS: a
// WKContentWorld). Defines:
//
//   __compassReader.check()  -> {ok, readerable}
//   __compassReader.parse({maxChars})
//       -> {ok, title, byline, siteName, excerpt, publishedTime, lang, dir,
//           url, content, length} or {ok: false, error}
//   __compassReader.sanitize(html, baseUrl) -> sanitized HTML (the reader
//       view sanitizes again with it)
//
// Tests pass another document as opts.document (and its URL as opts.url).
//
// `content` is the article's HTML after an allowlist: no scripts, styles,
// event handlers, frames, forms or media players; images and links keep
// absolute http(s) URLs. Nothing leaves the device: the result goes back to
// the browser, which shows it in its own reader view.

(() => {
  'use strict';
  if (globalThis.__compassReader) {
    return;
  }

  // Elements kept, with the attributes each may keep (beside kGlobalAttrs).
  const kAllowed = {
    a: ['href'],
    abbr: [], address: [], article: [], aside: [], b: [], bdi: [], bdo: [],
    blockquote: ['cite'], br: [], caption: [], cite: [], code: [],
    col: ['span'], colgroup: ['span'], dd: [], del: ['cite', 'datetime'],
    details: ['open'], dfn: [], div: [], dl: [], dt: [], em: [],
    figcaption: [], figure: [], footer: [], h1: [], h2: [], h3: [], h4: [],
    h5: [], h6: [], header: [], hr: [], i: [],
    img: ['src', 'srcset', 'sizes', 'alt', 'width', 'height'],
    ins: ['cite', 'datetime'], kbd: [], li: ['value'], main: [], mark: [],
    ol: ['start', 'reversed', 'type'], p: [], picture: [], pre: [],
    q: ['cite'], rp: [], rt: [], ruby: [], s: [], samp: [], section: [],
    small: [], source: ['srcset', 'sizes', 'media', 'type'], span: [],
    strong: [], sub: [], summary: [], sup: [], table: [], tbody: [],
    td: ['colspan', 'rowspan', 'headers'], tfoot: [],
    th: ['colspan', 'rowspan', 'headers', 'scope', 'abbr'], thead: [],
    time: ['datetime'], tr: [], u: [], ul: [], var: [], wbr: [],
  };
  const kGlobalAttrs = ['dir', 'lang', 'title'];
  // Dropped with everything in them. Anything else not allowed is unwrapped:
  // its children stay.
  const kDropped = new Set([
    'applet', 'audio', 'base', 'button', 'canvas', 'datalist', 'dialog',
    'embed', 'fieldset', 'form', 'frame', 'frameset', 'head', 'iframe',
    'input', 'link', 'map', 'math', 'meta', 'meter', 'noembed', 'noframes',
    'noscript', 'object', 'optgroup', 'option', 'output', 'param', 'portal',
    'progress', 'script', 'select', 'slot', 'style', 'svg', 'template',
    'textarea', 'title', 'track', 'video',
  ]);
  const kUrlAttrs = new Set(['href', 'src', 'cite']);

  // An absolute URL for |value|, or null if it isn't one the reader keeps:
  // http(s) everywhere, mailto for links, data: images.
  function safeUrl(value, base, attr, tag) {
    let url;
    try {
      url = new URL(value.trim(), base);
    } catch (e) {
      return null;
    }
    if (url.protocol === 'http:' || url.protocol === 'https:') {
      return url.href;
    }
    if (attr === 'href' && url.protocol === 'mailto:') {
      return url.href;
    }
    if (attr === 'src' && tag === 'img' && url.protocol === 'data:' &&
        /^data:image\/(png|gif|jpeg|webp|avif);/i.test(url.href)) {
      return url.href;
    }
    return null;
  }

  // "a.jpg 1x, b.jpg 2x" with each URL made absolute; null if none is safe.
  function safeSrcset(value, base) {
    const out = [];
    for (const part of value.split(/,(?=\s*\S)/)) {
      const [candidate, ...descriptor] = part.trim().split(/\s+/);
      if (!candidate) {
        continue;
      }
      const url = safeUrl(candidate, base, 'srcset', 'img');
      if (url && !url.startsWith('data:')) {
        out.push([url.replace(/,/g, '%2C'), ...descriptor].join(' '));
      }
    }
    return out.length ? out.join(', ') : null;
  }

  function cleanElement(element, base) {
    const tag = element.localName;
    const allowed = kAllowed[tag];
    for (const attr of [...element.attributes]) {
      const name = attr.name.toLowerCase();
      if (!kGlobalAttrs.includes(name) && !allowed.includes(name)) {
        element.removeAttribute(attr.name);
        continue;
      }
      if (kUrlAttrs.has(name)) {
        const url = safeUrl(attr.value, base, name, tag);
        if (url) {
          element.setAttribute(name, url);
        } else {
          element.removeAttribute(attr.name);
        }
      } else if (name === 'srcset') {
        const srcset = safeSrcset(attr.value, base);
        if (srcset) {
          element.setAttribute(name, srcset);
        } else {
          element.removeAttribute(attr.name);
        }
      } else if (name === 'dir' && !/^(ltr|rtl|auto)$/i.test(attr.value)) {
        element.removeAttribute(attr.name);
      }
    }
    if (tag === 'a' && element.hasAttribute('href')) {
      // The reader is another page: links open the site, without telling it
      // where from.
      element.setAttribute('rel', 'noopener noreferrer');
    }
  }

  function cleanTree(root, base) {
    // Children first-to-last; the list is copied since it changes.
    for (const node of [...root.childNodes]) {
      if (node.nodeType === Node.TEXT_NODE) {
        continue;
      }
      if (node.nodeType !== Node.ELEMENT_NODE) {
        // Comments, processing instructions, CDATA.
        node.remove();
        continue;
      }
      const tag = node.localName;
      if (node.namespaceURI !== 'http://www.w3.org/1999/xhtml' ||
          kDropped.has(tag)) {
        node.remove();
        continue;
      }
      cleanTree(node, base);
      if (!(tag in kAllowed)) {
        node.replaceWith(...node.childNodes);
        continue;
      }
      cleanElement(node, base);
      // An image left without a source shows nothing.
      if (tag === 'img' && !node.hasAttribute('src') &&
          !node.hasAttribute('srcset')) {
        node.remove();
      }
    }
  }

  // |html| through the allowlist, in an inert document: nothing in it loads
  // or runs while it's cleaned.
  function sanitize(html, base) {
    const doc = document.implementation.createHTMLDocument('');
    const container = doc.createElement('div');
    // An inert document's template parsing: scripts don't run, images don't
    // load.
    const template = doc.createElement('template');
    template.innerHTML = String(html);
    container.append(template.content);
    cleanTree(container, base || document.baseURI);
    return container.innerHTML;
  }

  function cleanText(value, max) {
    if (typeof value !== 'string') {
      return '';
    }
    const text = value.replace(/\s+/g, ' ').trim();
    return text.length > max ? text.slice(0, max - 1) + '…' : text;
  }

  function check(opts) {
    const doc = opts && opts.document || document;
    try {
      if (!doc.body || doc.contentType && !/html/.test(doc.contentType)) {
        return {ok: true, readerable: false};
      }
      return {ok: true, readerable: !!isProbablyReaderable(doc)};
    } catch (e) {
      return {ok: false, error: String(e && e.message || e)};
    }
  }

  function parse(opts) {
    opts = opts || {};
    const maxChars = opts.maxChars || opts.max_chars || 2000000;
    const doc = opts.document || document;
    try {
      if (!doc.documentElement) {
        return {ok: false, error: 'The page has no document.'};
      }
      const url = opts.document ? opts.url : location.href;
      const base = opts.document ? opts.url : doc.baseURI;
      // Readability changes the document it reads: a copy, so the page
      // itself stays as it is.
      const copy = doc.cloneNode(true);
      if (opts.document && opts.url && copy.head) {
        // A parsed document's base is the test page's.
        const baseElement = copy.createElement('base');
        baseElement.href = opts.url;
        copy.head.prepend(baseElement);
      }
      const article = new Readability(copy, {
        charThreshold: 300,
        keepClasses: false,
      }).parse();
      if (!article || !article.content) {
        return {ok: false, error: 'No article found on this page.'};
      }
      let content = sanitize(article.content, base);
      if (content.length > maxChars) {
        return {ok: false, error: 'The article is too long.'};
      }
      const dir = /^(ltr|rtl)$/i.test(article.dir || '') ?
          article.dir.toLowerCase() :
          (doc.dir || '').toLowerCase();
      return {
        ok: true,
        url,
        title: cleanText(article.title || doc.title, 500),
        byline: cleanText(article.byline, 300),
        siteName: cleanText(article.siteName, 200),
        excerpt: cleanText(article.excerpt, 1000),
        publishedTime: cleanText(article.publishedTime, 100),
        lang: cleanText(article.lang || doc.documentElement.lang, 35),
        dir: dir === 'rtl' ? 'rtl' : 'ltr',
        content,
        length: (article.textContent || '').trim().length,
      };
    } catch (e) {
      return {ok: false, error: String(e && e.message || e)};
    }
  }

  globalThis.__compassReader = {check, parse, sanitize};
})();
