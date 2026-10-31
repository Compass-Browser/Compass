// Run on chrome://bookmarks with --arg tag=NAME [--arg timeout=SECONDS].
// Waits (default 100 s) until the bookmark add_bookmark.js makes for NAME
// is here, and returns when it showed up (`at`, Date.now()), for timing
// sync without a manual refresh (docs/design/sync-slice-6-steps.md).
//
// getTree() is the snapshot taken when the page loaded (a promise resolved
// once), so new bookmarks come from the page's onCreated events instead.
const m = await import('chrome://bookmarks/bookmarks.js');
const api = m.BookmarksApiProxyImpl.getInstance();
const url = 'https://example.com/compass-sync-e2e/' + ARGS.tag;
const nodes = await api.getTree();
if (Object.values(nodes).some(n => n.url === url)) {
  return {found: ARGS.tag, at: Date.now(), already: true};
}
return await new Promise((resolve, reject) => {
  const listener = (parentId, index, node) => {
    if (node && node.url === url) {
      const at = Date.now();
      api.onCreated.removeListener(listener);
      clearTimeout(timer);
      resolve({found: ARGS.tag, at});
    }
  };
  const timer = setTimeout(() => {
    api.onCreated.removeListener(listener);
    reject(new Error('no bookmark ' + ARGS.tag + ' in time'));
  }, 1000 * Number(ARGS.timeout || 100));
  api.onCreated.addListener(listener);
});
