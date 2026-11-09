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
