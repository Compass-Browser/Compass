// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Live folder: the links on a page that match a pattern, or the entries of
// a JSON endpoint. "Use my login" sends the profile's cookies for that site
// (after the user grants it). See docs/design/live-folders.md.

function matcher(pattern) {
  const p = (pattern || '').trim();
  if (!p) return () => true;
  const re = /^\/(.+)\/([a-z]*)$/.exec(p);
  if (re) {
    let regex;
    try {
      regex = new RegExp(re[1], re[2].replace(/[gy]/g, ''));
    } catch (e) {
      throw new Error('The pattern is not a valid regular expression: ' + e.message);
    }
    return (s) => regex.test(s);
  }
  const needle = p.toLowerCase();
  return (s) => s.toLowerCase().indexOf(needle) >= 0;
}

// "data.children.0.title" → value; numbers index arrays. An empty path is
// the value itself.
function at(value, pathText) {
  const parts = (pathText || '').split('.').map((s) => s.trim()).filter(Boolean);
  let v = value;
  for (const p of parts) {
    if (v === null || v === undefined) return undefined;
    v = v[Array.isArray(v) && /^\d+$/.test(p) ? Number(p) : p];
  }
  return v;
}

function absolute(href, base) {
  if (typeof href !== 'string' || !href.trim()) return null;
  try {
    return new URL(href.trim(), base).href;
  } catch (e) {
    return null;
  }
}

async function fetchItems(config) {
  const res = await compass.fetch(config.url, {
    credentials: config.signedIn ? 'include' : 'omit',
    headers: {accept: config.mode === 'json' ? 'application/json' : 'text/html,*/*;q=0.8'},
  });
  if (!res.ok) throw new Error('The site answered HTTP ' + res.status);
  const body = await res.text();
  const base = res.url || config.url;
  const keep = matcher(config.match);

  if (config.mode === 'json') {
    let data;
    try {
      data = JSON.parse(body);
    } catch (e) {
      throw new Error('The response is not JSON');
    }
    const list = at(data, config.items);
    if (!Array.isArray(list)) throw new Error('No list at "' + (config.items || '(top level)') + '"');
    const items = [];
    for (const entry of list) {
      const url = absolute(at(entry, config.link), base);
      const title = at(entry, config.title);
      if (!url || title === undefined || title === null) continue;
      const titleText = compass.text.stripTags(String(title));
      if (!keep(url) && !keep(titleText)) continue;
      const id = config.id ? at(entry, config.id) : url;
      items.push({id: String(id === undefined || id === null ? url : id), title: titleText, url});
      if (items.length >= config.limit) break;
    }
    return {items};
  }

  const page = new URL(base);
  const items = compass.html.links(body, base)
      .filter((l) => l.text && (keep(l.url) || keep(l.text)))
      // Without a pattern, skip the page's own navigation (same URL, or
      // links to the site root).
      .filter((l) => config.match || (l.url !== page.href.replace(/#.*$/, '') &&
                                      !(new URL(l.url).origin === page.origin && new URL(l.url).pathname === '/')))
      .slice(0, config.limit)
      .map((l) => ({id: l.url, title: l.text, url: l.url}));
  return {title: compass.html.title(body), items};
}
