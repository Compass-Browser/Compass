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

  // A tab preview's result (docs/design/live-folders.md, "Tab previews"):
  // fewer items, `url` optional, a header and a footer, and icons only from
  // hosts the host allows, since loading an image is a request.
  const PREVIEW_LIMITS = Object.freeze({maxItems: 10, footerLength: 120, maxCount: 99999});

  // `options.preview`: normalize as a preview. `options.iconAllowed(url)`:
  // whether an icon may load (previews only; every icon is dropped without it).
  function normalizeResult(raw, limits, options) {
    const preview = !!(options && options.preview);
    const iconAllowed = (options && options.iconAllowed) || (() => !preview);
    const lim = Object.assign({}, LIMITS, preview ? PREVIEW_LIMITS : {}, limits || {});
    if (raw === null || typeof raw !== 'object' || !Array.isArray(raw.items)) {
      throw new ResultError((preview ? 'previewTab' : 'fetchItems') + ' must return {items: [...]}');
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
      if (!url && !(preview && (it.url === undefined || it.url === null))) {
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
      const item = url ? {id, title, url} : {id, title};
      if (typeof it.subtitle === 'string' && text.collapse(it.subtitle)) {
        item.subtitle = clip(it.subtitle, lim.subtitleLength);
      }
      if (it.icon !== undefined) {
        const icon = httpUrl(it.icon, lim.urlLength);
        if (!icon) warnings.push(where + ': icon must be an http(s) URL');
        else if (!iconAllowed(icon)) warnings.push(where + ': icon must be on the tab\'s site or a related host');
        else item.icon = icon;
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
    if (preview && typeof raw.footer === 'string' && text.collapse(raw.footer)) {
      result.footer = clip(raw.footer, lim.footerLength);
    }
    // The tab row's badge: how many (0 for none), or true for some.
    if (preview && raw.count !== undefined && raw.count !== null) {
      if (raw.count === true) {
        result.count = true;
      } else if (typeof raw.count === 'number' && isFinite(raw.count) && raw.count >= 0) {
        result.count = Math.min(Math.floor(raw.count), lim.maxCount);
      } else if (raw.count !== false) {
        warnings.push('count must be a number of 0 or more, or true');
      }
    }
    if (!preview && typeof raw.nextRefresh === 'number' && isFinite(raw.nextRefresh) && raw.nextRefresh > 0) {
      result.nextRefresh = Math.round(raw.nextRefresh);
    }
    result.warnings = warnings;
    return result;
  }

  const api = Object.freeze({LIMITS, PREVIEW_LIMITS, ResultError, normalizeResult});
  root.CompassLive = root.CompassLive || {};
  root.CompassLive.result = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
