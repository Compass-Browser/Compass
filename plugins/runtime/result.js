// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Checks and normalizes what a plugin's fetchItems() returned, outside the
// plugin's context. Bad items are dropped with a warning rather than
// failing the refresh; a result that isn't {items: [...]} fails it. This is
// the reference for docs/design/live-folders.md, "Result".

(function (root) {
  'use strict';

  const text = (root.CompassSDK && root.CompassSDK.text) ||
      (typeof require === 'function' ? require('../sdk/text.js') : null);

  const LIMITS = Object.freeze({
    maxItems: 50, idLength: 512, titleLength: 300, subtitleLength: 300,
    badgeLength: 12, urlLength: 2048, folderTitleLength: 100,
  });

  class ResultError extends Error {
    constructor(message) {
      super(message);
      this.name = 'ResultError';
      this.code = 'bad_result';
    }
  }

  function httpUrl(v, max) {
    if (typeof v !== 'string' || v.length > max) return null;
    try {
      const u = new URL(v);
      if (u.protocol !== 'http:' && u.protocol !== 'https:') return null;
      return u.href;
    } catch (e) {
      return null;
    }
  }

  function clip(s, max) {
    s = text.collapse(s);
    return s.length > max ? s.slice(0, max - 1) + '…' : s;
  }

  function normalizeResult(raw, limits) {
    const lim = Object.assign({}, LIMITS, limits || {});
    if (raw === null || typeof raw !== 'object' || !Array.isArray(raw.items)) {
      throw new ResultError('fetchItems must return {items: [...]}');
    }
    const warnings = [];
    const items = [];
    const seen = new Set();
    let truncated = false;
    raw.items.forEach((it, i) => {
      const where = 'items[' + i + ']';
      if (it === null || typeof it !== 'object') {
        warnings.push(where + ': not an object');
        return;
      }
      let id = typeof it.id === 'number' && isFinite(it.id) ? String(it.id) : it.id;
      if (typeof id !== 'string' || !id || id.length > lim.idLength) {
        warnings.push(where + ': id must be a non-empty string');
        return;
      }
      if (seen.has(id)) {
        warnings.push(where + ': duplicate id ' + JSON.stringify(id));
        return;
      }
      const url = httpUrl(it.url, lim.urlLength);
      if (!url) {
        warnings.push(where + ': url must be an http(s) URL');
        return;
      }
      const title = typeof it.title === 'string' ? clip(it.title, lim.titleLength) : '';
      if (!title) {
        warnings.push(where + ': title is required');
        return;
      }
      if (items.length >= lim.maxItems) {
        if (!truncated) warnings.push('more than ' + lim.maxItems + ' items; the rest are dropped');
        truncated = true;
        return;
      }
      seen.add(id);
      const item = {id, title, url};
      if (typeof it.subtitle === 'string' && text.collapse(it.subtitle)) {
        item.subtitle = clip(it.subtitle, lim.subtitleLength);
      }
      if (it.icon !== undefined) {
        const icon = httpUrl(it.icon, lim.urlLength);
        if (icon) item.icon = icon;
        else warnings.push(where + ': icon must be an http(s) URL');
      }
      if (it.time !== undefined && it.time !== null) {
        const t = typeof it.time === 'number' ? it.time : text.parseDate(it.time);
        if (typeof t === 'number' && isFinite(t)) item.time = Math.round(t);
        else warnings.push(where + ': time is not a date');
      }
      if (typeof it.badge === 'string' && text.collapse(it.badge)) {
        item.badge = clip(it.badge, lim.badgeLength);
      }
      items.push(item);
    });

    const result = {items};
    if (typeof raw.title === 'string' && text.collapse(raw.title)) {
      result.title = clip(raw.title, lim.folderTitleLength);
    }
    if (typeof raw.nextRefresh === 'number' && isFinite(raw.nextRefresh) && raw.nextRefresh > 0) {
      result.nextRefresh = Math.round(raw.nextRefresh);
    }
    result.warnings = warnings;
    return result;
  }

  const api = Object.freeze({LIMITS, ResultError, normalizeResult});
  root.CompassLive = root.CompassLive || {};
  root.CompassLive.result = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
