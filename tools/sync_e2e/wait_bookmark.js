// Run on chrome://bookmarks with --arg tag=NAME [--arg timeout=SECONDS].
// Waits (polling every 100 ms, default 100 s) until the bookmark
// add_bookmark.js makes for NAME is here, and returns when it showed up
// (`at`, Date.now()), for timing sync without a manual refresh
// (docs/design/sync-slice-6-steps.md).
const m = await import('chrome://bookmarks/bookmarks.js');
const api = m.BookmarksApiProxyImpl.getInstance();
const url = 'https://example.com/compass-sync-e2e/' + ARGS.tag;
const deadline = Date.now() + 1000 * Number(ARGS.timeout || 100);
while (Date.now() < deadline) {
  const nodes = await api.getTree();
  if (Object.values(nodes).some(n => n.url === url)) {
    return {found: ARGS.tag, at: Date.now()};
  }
  await new Promise(r => setTimeout(r, 100));
}
throw new Error('no bookmark ' + ARGS.tag + ' in time');
