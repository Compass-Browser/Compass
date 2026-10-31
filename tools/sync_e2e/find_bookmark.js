// Run on chrome://bookmarks. Lists the bookmarks add_bookmark.js made.
const m = await import('chrome://bookmarks/bookmarks.js');
const nodes = await m.BookmarksApiProxyImpl.getInstance().getTree();
return Object.values(nodes)
    .filter(n => n.url && n.url.includes('compass-sync-e2e'))
    .map(n => ({title: n.title, url: n.url,
                parent: (nodes[n.parentId] || {}).title}));
