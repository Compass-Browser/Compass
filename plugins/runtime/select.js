// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tab previews' selector groups (docs/design/live-folders.md, "Tab
// previews"): what a manifest's preview.select reads from a document. Host
// code, never the plugin's: the browser runs it in an isolated world of the
// hovered tab (the snapshot, and requests with the tab's session), and the
// chrome-untrusted://live-folders/ page runs it on HTML a session request
// fetched (`compass.fetch(url, {select})`), so both give the same shape:
//
//   {<group>: [{attrs?: {name: value}, text?: string, fields?: {name: value}}]}
//
// `href` and `src` values are made absolute against the document's URL;
// text is collapsed and capped.

(function (root) {
  'use strict';

  // The browser sends this script with every call into a tab: install once
  // per document, so requests in flight survive.
  if (root.CompassLive && root.CompassLive.select) return;

  const MAX_TEXT = 1000;
  const URL_ATTRS = new Set(['href', 'src', 'action', 'poster', 'data-href', 'data-url']);

  function collapse(s) {
    s = String(s || '').replace(/\s+/g, ' ').trim();
    return s.length > MAX_TEXT ? s.slice(0, MAX_TEXT - 1) + '…' : s;
  }

  function attrValue(el, name, baseUrl) {
    const v = el.getAttribute(name);
    if (v === null) return undefined;
    if (!URL_ATTRS.has(name.toLowerCase())) return collapse(v);
    try {
      const u = new URL(v, baseUrl);
      return u.protocol === 'http:' || u.protocol === 'https:' ? u.href : undefined;
    } catch (e) {
      return undefined;
    }
  }

  // "selector", "selector@attribute" or "@attribute".
  function fieldValue(el, spec, baseUrl) {
    const at = spec.indexOf('@');
    const selector = (at < 0 ? spec : spec.slice(0, at)).trim();
    const attr = at < 0 ? '' : spec.slice(at + 1);
    const target = selector ? el.querySelector(selector) : el;
    if (!target) return undefined;
    return attr ? attrValue(target, attr, baseUrl) : collapse(target.textContent) || undefined;
  }

  function node(el, group, baseUrl) {
    const out = {};
    if (group.attrs && group.attrs.length) {
      const attrs = {};
      for (const name of group.attrs) {
        const v = attrValue(el, name, baseUrl);
        if (v !== undefined) attrs[name] = v;
      }
      out.attrs = attrs;
    }
    if (group.text) out.text = collapse(el.textContent);
    const specs = Object.entries(group.fields || {});
    if (specs.length) {
      const fields = {};
      for (const [name, spec] of specs) {
        let v;
        try {
          v = fieldValue(el, spec, baseUrl);
        } catch (e) {
          v = undefined;  // An invalid selector reads nothing.
        }
        if (v !== undefined) fields[name] = v;
      }
      out.fields = fields;
    }
    return out;
  }

  // Applies `groups` ({name: {selector, attrs, text, fields, max}}) to
  // `doc` (a Document or Element).
  function apply(doc, groups, baseUrl) {
    const out = {};
    for (const [name, group] of Object.entries(groups || {})) {
      let elements = [];
      try {
        elements = Array.from(doc.querySelectorAll(group.selector));
      } catch (e) {
        elements = [];
      }
      out[name] = elements.slice(0, group.max || 50).map((el) => node(el, group, baseUrl));
    }
    return out;
  }

  // HTML a session request fetched, parsed without running its scripts.
  function applyToHtml(html, groups, baseUrl) {
    const doc = new DOMParser().parseFromString(String(html), 'text/html');
    return apply(doc, groups, baseUrl);
  }

  // In the hovered tab: what previewTab() gets as `page`, before the browser
  // caps it. opts: {groups, pageText, pageCookies}.
  function snapshot(opts) {
    const page = {
      url: location.href,
      title: document.title,
      live: true,
      groups: apply(document, opts.groups || {}, location.href),
    };
    if (opts.pageText && document.body) {
      page.text = String(document.body.innerText || '').slice(0, 50000);
    }
    if (opts.pageCookies && opts.pageCookies.length) {
      const wanted = new Set(opts.pageCookies);
      const cookies = {};
      for (const part of String(document.cookie || '').split(';')) {
        const eq = part.indexOf('=');
        const name = (eq < 0 ? part : part.slice(0, eq)).trim();
        if (wanted.has(name)) cookies[name] = part.slice(eq + 1).trim();
      }
      page.cookies = cookies;
    }
    return page;
  }

  // Requests with the tab's session, from its isolated world, so they carry
  // the page's origin and cookies. Script results can't wait for a promise,
  // so the browser starts one and then asks for its result.
  const fetches = new Map();

  function startFetch(id, req) {
    const init = {
      method: req.method || 'GET',
      headers: req.headers || {},
      credentials: 'include',
      // Redirects come back to the browser, which checks each hop.
      redirect: 'manual',
      cache: 'no-store',
    };
    if (req.body !== null && req.body !== undefined && init.method === 'POST') init.body = req.body;
    const entry = {done: false};
    fetches.set(id, entry);
    fetch(req.url, init).then(async (res) => {
      if (res.type === 'opaqueredirect') {
        entry.result = {redirect: true};
        return;
      }
      const text = await res.text();
      const headers = {};
      res.headers.forEach((v, k) => { headers[k] = v; });
      entry.result = text.length > req.maxBytes
          ? {error: 'response_too_large'}
          : {status: res.status, url: res.url, headers, body: text};
    }, (e) => {
      entry.result = {error: 'network', message: String(e && e.message || e)};
    }).finally(() => { entry.done = true; });
    return {started: true};
  }

  // {pending: true} until the request is done, then its result, once.
  function fetchResult(id) {
    const entry = fetches.get(id);
    if (!entry) return {error: 'network', message: 'No such request'};
    if (!entry.done) return {pending: true};
    fetches.delete(id);
    return entry.result;
  }

  const api = Object.freeze({apply, applyToHtml, snapshot, startFetch, fetchResult});
  root.CompassLive = root.CompassLive || {};
  root.CompassLive.select = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
