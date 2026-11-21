// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Live folder: GitHub pull requests and issues from the search API. Needs a
// personal access token (api.github.com doesn't accept the github.com
// session cookie). See docs/design/live-folders.md.

const QUERIES = {
  'review-requested': 'is:pr is:open archived:false review-requested:@me',
  'authored': 'is:pr is:open archived:false author:@me',
  'assigned': 'is:open archived:false assignee:@me',
  'mentioned': 'is:open archived:false mentions:@me',
};

function searchQuery(config) {
  let q;
  if (config.show === 'search') {
    q = (config.query || '').trim();
    if (!q) throw new Error('Enter a search query, or pick another view');
  } else {
    q = QUERIES[config.show];
  }
  const scope = (config.repo || '').trim().replace(/^https:\/\/github\.com\//, '').replace(/\/+$/, '');
  if (scope) q += (scope.indexOf('/') > 0 ? ' repo:' : ' user:') + scope;
  return q;
}

async function fetchItems(config) {
  const q = searchQuery(config);
  const url = 'https://api.github.com/search/issues?q=' + encodeURIComponent(q) +
      '&sort=updated&order=desc&per_page=' + config.limit;
  const res = await compass.fetch(url, {
    headers: {
      accept: 'application/vnd.github+json',
      authorization: 'Bearer ' + compass.secret('token'),
      'x-github-api-version': '2022-11-28',
    },
  });
  if (res.status === 401) throw new Error('GitHub rejected the token. Check it in the folder\'s settings.');
  if (res.status === 403 || res.status === 429) throw new Error('GitHub rate limit reached; trying again later.');
  if (!res.ok) throw new Error('GitHub answered HTTP ' + res.status);
  const data = await res.json();

  const items = (data.items || []).map((it) => {
    const repo = String(it.repository_url || '').replace('https://api.github.com/repos/', '');
    const isPr = !!it.pull_request;
    return {
      id: it.html_url,
      title: it.title,
      url: it.html_url,
      subtitle: repo + '#' + it.number + (isPr ? '' : ' (issue)'),
      time: it.updated_at,
      badge: it.draft ? 'Draft' : undefined,
    };
  });
  return {items};
}

// ---- Tab preview: recent notifications ----------------------------------
//
// On github.com/notifications the list comes from the page itself. On any
// other GitHub tab, or a discarded one, it needs the user's session: the
// notifications page is fetched (api.github.com doesn't take the session
// cookie, so the REST API isn't an option) and the host applies the same
// selector group to it. See docs/design/live-folders.md, "Tab previews".

const NOTIFICATIONS = 'https://github.com/notifications';

const TYPES = [
  [/octicon-git-(pull-request|merge)/, 'PR'],
  [/octicon-issue/, 'Issue'],
  [/octicon-(tag|rocket|package)/, 'Release'],
  [/octicon-(x-circle|check-circle|stop|play|workflow|alert)/, 'CI'],
  [/octicon-comment-discussion/, 'Discussion'],
  [/octicon-git-commit/, 'Commit'],
  [/octicon-shield/, 'Security'],
];

function typeOf(svgClass) {
  for (const [re, label] of TYPES) {
    if (re.test(svgClass || '')) return label;
  }
  return undefined;
}

function isUnread(node) {
  return /\bnotification-unread\b/.test((node.attrs && node.attrs.class) || '');
}

const REASONS = {
  'review requested': 'Review requested',
  'mention': 'Mentioned',
  'team mention': 'Team mentioned',
  'author': 'Yours',
  'assign': 'Assigned',
  'comment': 'Commented',
  'ci activity': 'CI',
  'state change': 'State changed',
  'subscribed': 'Watching',
  'security alert': 'Security alert',
};

function reasonLabel(s) {
  const key = (s || '').toLowerCase();
  if (REASONS[key]) return REASONS[key];
  return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
}

function notificationItem(node) {
  const f = node.fields || {};
  const url = f.href;
  const title = compass.text.collapse(f.title);
  if (!url || !title) return null;
  const repo = compass.text.collapse(f.repo);
  const reason = reasonLabel(compass.text.collapse(f.reason));
  const item = {
    // The referrer id changes; the thread doesn't.
    id: url.split('?')[0],
    title,
    url,
    subtitle: [repo, reason].filter(Boolean).join(' · ') || undefined,
    time: f.time || undefined,
    badge: typeOf(f.type),
  };
  if (f.avatar) item.icon = f.avatar;
  return item;
}

function onNotificationsPage(page) {
  return page.live && /^https:\/\/github\.com\/notifications(?:[/?#]|$)/.test(page.url);
}

// The Inbox counter on the notifications page: how many are unread.
function inboxCount(groups) {
  const text = groups && groups.counter && groups.counter[0] && groups.counter[0].text;
  const n = parseInt(String(text || '').replace(/[^\d]/g, ''), 10);
  return isFinite(n) ? n : null;
}

// Any other GitHub page's header only says whether some are unread.
function headerSaysUnread(groups) {
  const text = groups && groups.inbox && groups.inbox[0] && groups.inbox[0].text;
  return !!text && /unread/i.test(text) && !/\bno unread\b/i.test(text);
}

async function previewTab(page, config) {
  let nodes = null;
  let count = null;
  if (onNotificationsPage(page)) {
    nodes = page.groups.notifications;
    count = inboxCount(page.groups);
  } else if (!compass.hasSession) {
    // The tab's badge still shows that something is waiting.
    return {
      items: [],
      count: headerSaysUnread(page.groups) ? true : 0,
      footer: config.hint ? 'To see notifications here, let GitHub use your session in Settings › Plugins' : undefined,
    };
  } else {
    const res = await compass.fetch(NOTIFICATIONS + (config.unreadOnly ? '?query=is%3Aunread' : ''),
                                    {select: ['notifications', 'counter']});
    // Signed out: GitHub sends the login page.
    if (/^https:\/\/github\.com\/(login|session)\b/.test(res.url)) return {items: [], count: 0};
    if (!res.ok) throw new Error('GitHub answered HTTP ' + res.status);
    nodes = res.groups.notifications;
    count = inboxCount(res.groups);
  }

  const relevant = config.unreadOnly ? nodes.filter(isUnread) : nodes;
  const items = [];
  for (const node of relevant) {
    const item = notificationItem(node);
    if (item) items.push(item);
    if (items.length >= config.count) break;
  }
  const unread = count !== null ? count : nodes.filter(isUnread).length;
  return {
    items,
    title: 'Notifications',
    count: unread,
    footer: unread ? unread + ' unread' : (items.length ? undefined : 'No unread notifications'),
  };
}
