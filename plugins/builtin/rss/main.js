// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Live folder: the latest posts of an RSS 2.0, RSS 1.0 (RDF), Atom or JSON
// Feed. See docs/design/live-folders.md.

function absolute(href, base) {
  if (!href || !href.trim()) return null;
  try {
    return new URL(href.trim(), base).href;
  } catch (e) {
    return null;
  }
}

function fromJsonFeed(feed, base) {
  const items = (Array.isArray(feed.items) ? feed.items : []).map((it) => ({
    id: String(it.id || it.url || ''),
    title: compass.text.stripTags(it.title || it.summary || it.content_text || ''),
    url: absolute(String(it.url || it.external_url || ''), base),
    time: it.date_published || it.date_modified,
  }));
  return {title: compass.text.collapse(feed.title || ''), items};
}

function atomLink(entry) {
  const links = entry.elements('link');
  const link = links.find((l) => !l.attr('rel') || l.attr('rel') === 'alternate') || links[0];
  return link ? link.attr('href') : null;
}

function fromXml(doc, base) {
  const feed = doc.child('feed');
  if (feed) {
    const items = feed.elements('entry').map((e) => {
      const href = atomLink(e);
      return {
        id: e.childText('id') || href || '',
        title: compass.text.stripTags(e.childText('title')),
        url: href ? absolute(href, base) : null,
        time: e.childText('published') || e.childText('updated'),
      };
    });
    return {title: compass.text.stripTags(feed.childText('title')), items};
  }

  // RSS 2.0 puts items in <channel>; RSS 1.0 puts them beside it.
  const top = doc.child('rss') || doc.child('RDF');
  if (!top) throw new Error('This is not an RSS, Atom or JSON feed');
  const channel = top.child('channel');
  const nodes = (channel ? channel.elements('item') : []).concat(top.elements('item'));
  const items = nodes.map((it) => {
    const guid = it.child('guid');
    const permalink = guid && guid.attr('isPermaLink') !== 'false' ? guid.text() : '';
    const link = it.childText('link') || permalink || it.attr('about') || '';
    return {
      id: (guid && guid.text()) || link,
      title: compass.text.stripTags(it.childText('title')) ||
          compass.text.stripTags(it.childText('description')).slice(0, 120),
      url: link ? absolute(link, base) : null,
      time: it.childText('pubDate') || it.childText('date') || it.childText('published'),
    };
  });
  return {title: channel ? compass.text.stripTags(channel.childText('title')) : '', items};
}

async function fetchItems(config) {
  const res = await compass.fetch(config.url, {
    headers: {accept: 'application/rss+xml, application/atom+xml, application/feed+json, application/xml;q=0.9, */*;q=0.8'},
  });
  if (!res.ok) throw new Error('The feed answered HTTP ' + res.status);
  const body = await res.text();
  const base = res.url || config.url;
  const parsed = /^\s*\{/.test(body) ? fromJsonFeed(JSON.parse(body), base) : fromXml(compass.xml.parse(body), base);

  const items = parsed.items
      .filter((it) => it.url && it.id && it.title)
      .map((it) => ({id: it.id, title: it.title, url: it.url, time: compass.text.parseDate(it.time) || undefined}));
  // Newest first when the feed has dates; feed order otherwise.
  if (items.every((it) => it.time)) items.sort((a, b) => b.time - a.time);
  return {title: parsed.title, items: items.slice(0, config.limit)};
}
