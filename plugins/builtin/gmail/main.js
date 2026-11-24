// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tab preview and badge: unread mail on Gmail tabs. Preview only (no live
// folder). See docs/design/live-folders.md, "Tab previews" and "Badges".
//
// The count comes from the page: the Inbox link in Gmail's side menu, else
// the tab's title ("Inbox (3) - …") while it shows the inbox, so a
// discarded tab keeps a count too. The rows come from the message list
// while the tab shows one; otherwise, with the session, from Gmail's Atom
// feed of unread inbox mail (mail/u/N/feed/atom), which also gives the
// exact unread count.

// The account's base URL: mail.google.com/mail/u/<n>/.
function accountBase(url) {
  const m = /^https:\/\/mail\.google\.com\/mail\/u\/(\d+)\//.exec(url || '');
  return 'https://mail.google.com/mail/u/' + (m ? m[1] : '0') + '/';
}

// The list the tab shows: "#inbox", "#label/Work"… (the inbox if none).
function viewOf(url) {
  const hash = /#([^?]*)$/.exec(url || '');
  const view = hash ? hash[1].split('/') : [];
  if (!view.length || !view[0]) return 'inbox';
  // "#label/Work/FMfcg…" is a conversation: its list is "#label/Work".
  return view[0] === 'label' || view[0] === 'category' ? view.slice(0, 2).join('/') : view[0];
}

function number(s) {
  const n = parseInt(String(s || '').replace(/[^\d]/g, ''), 10);
  return isFinite(n) ? n : null;
}

// The side menu's Inbox link (Gmail's logo also links to #inbox, outside
// the menu): "Inbox 24 unread", "Posta in arrivo 5 da leggere"; just
// "Inbox" when none is unread.
function menuCount(groups) {
  const link = groups && groups.inbox && groups.inbox[0];
  if (!link) return null;
  const n = number(link.attrs && link.attrs['aria-label']);
  return n !== null ? n : 0;
}

// "Inbox (3) - me@example.com - Gmail", in any language, while the tab
// shows the inbox; another view's title counts something else.
function titleCount(page) {
  if (viewOf(page.url) !== 'inbox') return null;
  const first = String(page.title || '').split(' - ')[0];
  const m = /\(([\d.,\s]+)\)\s*$/.exec(first);
  if (m) return number(m[1]);
  return / - Gmail$/.test(page.title || '') ? 0 : null;
}

function isUnread(row) {
  return /\bzE\b/.test((row.attrs && row.attrs.class) || '');
}

function rowItem(row, page) {
  const f = row.fields || {};
  const subject = compass.text.collapse(f.subject) || '(no subject)';
  const sender = compass.text.collapse(f.name) || compass.text.collapse(f.sender) ||
      compass.text.collapse(f.email);
  const thread = f.thread;
  const id = thread || (sender + '|' + subject + '|' + (f.date || ''));
  const item = {
    id,
    title: subject,
    subtitle: [sender, compass.text.collapse(f.when)].filter(Boolean).join(' · ') || undefined,
  };
  if (thread) item.url = accountBase(page.url) + '#' + viewOf(page.url) + '/' + thread;
  return item;
}

function feedItem(entry) {
  const title = entry.childText('title') || '(no subject)';
  const link = entry.child('link');
  const url = link && link.attr('href');
  const author = entry.child('author');
  const sender = author && (author.childText('name') || author.childText('email'));
  const item = {
    id: entry.childText('id') || url || title,
    title,
    subtitle: sender || undefined,
    time: entry.childText('issued') || entry.childText('modified') || undefined,
  };
  if (url && /^https:\/\//.test(url)) item.url = url;
  return item;
}

// "3 unread", or "3 unread in Inbox" under another view's rows.
function footerFor(count, shown, elsewhere) {
  if (count) return count.toLocaleString(compass.locale) + ' unread' + (elsewhere ? ' in Inbox' : '');
  return shown ? undefined : 'No unread mail';
}

async function previewTab(page, config) {
  const rows = page.groups.rows || [];
  let count = menuCount(page.groups);
  if (count === null) count = titleCount(page);

  // The tab shows a message list: read it.
  if (page.live && rows.length) {
    const inInbox = viewOf(page.url) === 'inbox';
    const relevant = config.unreadOnly ? rows.filter(isUnread) : rows;
    const items = relevant.slice(0, config.count).map((row) => rowItem(row, page));
    if (count === null && inInbox) count = rows.filter(isUnread).length;
    return {
      items,
      title: inInbox ? 'Inbox' : undefined,
      count: count === null ? 0 : count,
      footer: footerFor(count, items.length, !inInbox),
    };
  }

  if (!compass.hasSession) {
    return {
      items: [],
      count: count === null ? 0 : count,
      footer: count && config.hint
        ? 'To see unread mail here, let Gmail use your session in Settings › Plugins'
        : undefined,
    };
  }

  // Unread inbox mail from the feed, with the user's session.
  const res = await compass.fetch(accountBase(page.url) + 'feed/atom');
  // Signed out: Gmail answers 401, or sends the sign-in page.
  if (res.status === 401 || res.status === 403 || !/^https:\/\/mail\.google\.com\//.test(res.url)) {
    return {items: [], count: count === null ? 0 : count};
  }
  if (!res.ok) throw new Error('Gmail answered HTTP ' + res.status);
  const doc = compass.xml.parse(await res.text());
  const feed = doc.find('feed');
  if (!feed) throw new Error('Gmail\'s unread feed has changed');
  const entries = feed.elements('entry');
  const unread = number(feed.childText('fullcount'));
  const total = unread !== null ? unread : entries.length;
  const items = entries.slice(0, config.count).map(feedItem);
  return {items, title: 'Inbox', count: total, footer: footerFor(total, items.length)};
}
