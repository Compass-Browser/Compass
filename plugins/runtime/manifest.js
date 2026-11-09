// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Live folder plugin manifests: validation, config validation and the host
// allowlist. This is the reference implementation of the rules in
// docs/design/live-folders.md ("Manifest", "Network access"). Browser hosts
// enforce the same rules natively (a plugin can't be trusted to check
// itself); tools/live_folders uses this file directly.

(function (root) {
  'use strict';

  const API_VERSION = 1;
  const REFRESH_FLOOR = 300;      // Seconds. No plugin refreshes faster.
  const REFRESH_DEFAULT = 900;
  const FIELD_TYPES = ['string', 'url', 'number', 'bool', 'select'];
  const PLACEHOLDER = /^\{(config|secret):([A-Za-z][A-Za-z0-9_]*)\}$/;
  const HOST = /^(\*\.)?([a-z0-9]([a-z0-9-]*[a-z0-9])?\.)*[a-z0-9]([a-z0-9-]*[a-z0-9])?$/;

  class ManifestError extends Error {
    constructor(errors) {
      super(errors.join('; '));
      this.name = 'ManifestError';
      this.code = 'bad_manifest';
      this.errors = errors;
    }
  }

  const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
  const isString = (v) => typeof v === 'string' && v.length > 0;

  // Returns a normalized copy of `m` (defaults filled in) or throws a
  // ManifestError listing every problem.
  function validateManifest(m) {
    const errors = [];
    if (!isObject(m)) throw new ManifestError(['manifest must be an object']);
    if (!isString(m.id) || !/^[a-z0-9]+(\.[a-z0-9][a-z0-9-]*)+$/.test(m.id) || m.id.length > 100) {
      errors.push('id must be a reverse-DNS style name like "com.example.feed"');
    }
    if (!isString(m.name) || m.name.length > 60) errors.push('name must be 1-60 characters');
    if (!isString(m.version) || !/^\d+(\.\d+){0,3}$/.test(m.version)) {
      errors.push('version must look like "1.2.3"');
    }
    if (m.api !== API_VERSION) errors.push('api must be ' + API_VERSION);
    const entry = m.entry === undefined ? 'main.js' : m.entry;
    if (!isString(entry) || !/^[A-Za-z0-9_\-.]+\.js$/.test(entry) || entry.startsWith('.')) {
      errors.push('entry must be a .js file name in the plugin folder');
    }
    if (m.icon !== undefined && (!isString(m.icon) || !/^[A-Za-z0-9_\-.]+\.(svg|png)$/.test(m.icon))) {
      errors.push('icon must be an .svg or .png file name in the plugin folder');
    }
    if (m.description !== undefined && (typeof m.description !== 'string' || m.description.length > 300)) {
      errors.push('description must be at most 300 characters');
    }

    const config = Array.isArray(m.config) ? m.config : (m.config === undefined ? [] : null);
    if (config === null) errors.push('config must be a list of fields');
    const keys = new Set();
    for (const [i, f] of (config || []).entries()) {
      const where = 'config[' + i + ']';
      if (!isObject(f)) {
        errors.push(where + ' must be an object');
        continue;
      }
      if (!isString(f.key) || !/^[A-Za-z][A-Za-z0-9_]*$/.test(f.key)) errors.push(where + '.key is invalid');
      else if (keys.has(f.key)) errors.push(where + '.key "' + f.key + '" is repeated');
      else keys.add(f.key);
      if (FIELD_TYPES.indexOf(f.type) < 0) errors.push(where + '.type must be one of ' + FIELD_TYPES.join(', '));
      if (!isString(f.label)) errors.push(where + '.label is required');
      if (f.type === 'select') {
        if (!Array.isArray(f.options) || f.options.length === 0) {
          errors.push(where + '.options is required for a select');
        } else if (!f.options.every((o) => isString(o) || (isObject(o) && isString(o.value) && isString(o.label)))) {
          errors.push(where + '.options must be strings or {value, label}');
        }
      }
    }

    const secrets = Array.isArray(m.secrets) ? m.secrets : (m.secrets === undefined ? [] : null);
    if (secrets === null) errors.push('secrets must be a list');
    const secretKeys = new Set();
    for (const [i, s] of (secrets || []).entries()) {
      if (!isObject(s) || !isString(s.key) || !/^[A-Za-z][A-Za-z0-9_]*$/.test(s.key) || !isString(s.label)) {
        errors.push('secrets[' + i + '] needs a key and a label');
      } else if (secretKeys.has(s.key)) {
        errors.push('secrets[' + i + '].key "' + s.key + '" is repeated');
      } else {
        secretKeys.add(s.key);
      }
    }

    const checkHosts = (list, what) => {
      if (list === undefined) return [];
      if (!Array.isArray(list)) {
        errors.push(what + ' must be a list');
        return [];
      }
      for (const h of list) {
        const p = typeof h === 'string' ? PLACEHOLDER.exec(h) : null;
        if (p) {
          const known = p[1] === 'config' ? keys : secretKeys;
          const field = p[1] === 'config' ? (config || []).find((f) => f.key === p[2]) : null;
          if (!known.has(p[2])) errors.push(what + ': ' + h + ' names an unknown ' + p[1] + ' key');
          else if (field && field.type !== 'url') errors.push(what + ': ' + h + ' must name a url field');
        } else if (typeof h !== 'string' || !HOST.test(h)) {
          errors.push(what + ': "' + h + '" is not a host name, "*.domain" or a {config:key} placeholder');
        }
      }
      return list.slice();
    };
    const hosts = checkHosts(m.hosts, 'hosts');
    const cookies = checkHosts(m.cookies, 'cookies');
    for (const c of cookies) {
      if (hosts.indexOf(c) < 0) errors.push('cookies: "' + c + '" must also be listed in hosts');
    }

    let refresh = {default: REFRESH_DEFAULT, min: REFRESH_FLOOR};
    if (m.refresh !== undefined) {
      if (!isObject(m.refresh)) {
        errors.push('refresh must be an object');
      } else {
        const min = m.refresh.min === undefined ? REFRESH_FLOOR : m.refresh.min;
        const def = m.refresh.default === undefined ? Math.max(min, REFRESH_DEFAULT) : m.refresh.default;
        if (typeof min !== 'number' || typeof def !== 'number' || def < min) {
          errors.push('refresh.default and refresh.min must be numbers, default >= min');
        }
        refresh = {default: Math.max(REFRESH_FLOOR, def), min: Math.max(REFRESH_FLOOR, min)};
      }
    }

    if (errors.length) throw new ManifestError(errors);
    return {
      id: m.id, name: m.name, version: m.version, api: m.api, entry,
      icon: m.icon || null, description: m.description || '',
      hosts, cookies,
      secrets: (secrets || []).map((s) => ({key: s.key, label: s.label, optional: !!s.optional,
                                             help: typeof s.help === 'string' ? s.help : ''})),
      config: (config || []).map((f) => Object.assign({}, f, {optional: !!f.optional})),
      refresh,
    };
  }

  function isHttpUrl(s) {
    try {
      const u = new URL(s);
      return u.protocol === 'http:' || u.protocol === 'https:';
    } catch (e) {
      return false;
    }
  }

  // Checks values against the manifest's fields and fills in defaults.
  // Unknown keys are dropped. Throws a ManifestError (code 'bad_config').
  function validateConfig(manifest, config) {
    const errors = [];
    const out = {};
    const input = isObject(config) ? config : {};
    for (const f of manifest.config) {
      let v = input[f.key];
      if (v === undefined || v === null || v === '') v = f.default;
      if (v === undefined || v === null || v === '') {
        if (!f.optional && f.type !== 'bool') errors.push(f.label + ' is required');
        if (f.type === 'bool') out[f.key] = false;
        continue;
      }
      switch (f.type) {
        case 'string':
          if (typeof v !== 'string') errors.push(f.label + ' must be text');
          break;
        case 'url':
          if (typeof v !== 'string' || !isHttpUrl(v)) errors.push(f.label + ' must be an http(s) URL');
          break;
        case 'number':
          if (typeof v === 'string' && v.trim() !== '') v = Number(v);
          if (typeof v !== 'number' || !isFinite(v)) errors.push(f.label + ' must be a number');
          else if ((f.min !== undefined && v < f.min) || (f.max !== undefined && v > f.max)) {
            errors.push(f.label + ' must be between ' + f.min + ' and ' + f.max);
          }
          break;
        case 'bool':
          if (v === 'true') v = true;
          if (v === 'false') v = false;
          if (typeof v !== 'boolean') errors.push(f.label + ' must be true or false');
          break;
        case 'select': {
          const values = f.options.map((o) => (typeof o === 'string' ? o : o.value));
          if (values.indexOf(v) < 0) errors.push(f.label + ' must be one of ' + values.join(', '));
          break;
        }
      }
      out[f.key] = v;
    }
    if (errors.length) {
      const e = new ManifestError(errors);
      e.code = 'bad_config';
      throw e;
    }
    return out;
  }

  // Resolves {config:key} / {secret:key} placeholders to host names.
  // Returns {hosts: [...], cookies: [...]} of plain patterns.
  function resolveHosts(manifest, config, secrets) {
    const resolve = (list) => {
      const out = [];
      for (const h of list) {
        const p = PLACEHOLDER.exec(h);
        if (!p) {
          out.push(h);
          continue;
        }
        const v = p[1] === 'config' ? (config || {})[p[2]] : (secrets || {})[p[2]];
        if (typeof v !== 'string') continue;
        try {
          const u = new URL(v.replace(/^webcal:/i, 'https:'));
          if (u.hostname) out.push(u.hostname.toLowerCase());
        } catch (e) { /* Not a URL: grants nothing. */ }
      }
      return out;
    };
    return {hosts: resolve(manifest.hosts), cookies: resolve(manifest.cookies)};
  }

  function hostMatches(pattern, hostname) {
    hostname = String(hostname).toLowerCase().replace(/\.$/, '');
    if (pattern.startsWith('*.')) {
      const domain = pattern.slice(2);
      return hostname === domain || hostname.endsWith('.' + domain);
    }
    return hostname === pattern;
  }

  // Whether a plugin may fetch `url` (http or https on an allowed host).
  function urlAllowed(url, patterns) {
    let u;
    try {
      u = new URL(url);
    } catch (e) {
      return false;
    }
    if (u.protocol !== 'http:' && u.protocol !== 'https:') return false;
    if (u.username || u.password) return false;
    return patterns.some((p) => hostMatches(p, u.hostname));
  }

  const api = Object.freeze({
    API_VERSION, REFRESH_FLOOR, ManifestError,
    validateManifest, validateConfig, resolveHosts, hostMatches, urlAllowed,
  });
  root.CompassLive = root.CompassLive || {};
  root.CompassLive.manifest = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
