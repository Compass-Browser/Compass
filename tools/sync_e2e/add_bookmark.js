// Run on chrome://bookmarks with --arg tag=NAME. Adds a bookmark
// "Compass sync NAME" (https://example.com/compass-sync-e2e/NAME) to the
// bookmarks bar.
const m = await import('chrome://bookmarks/bookmarks.js');
const api = m.BookmarksApiProxyImpl.getInstance();
const nodes = await api.getTree();
const root = Object.values(nodes).find(n => !n.parentId);
const bar = root.children[0];
const made = await api.create(
    bar, null, 'Compass sync ' + ARGS.tag,
    'https://example.com/compass-sync-e2e/' + ARGS.tag);
return {parent: nodes[bar].title, created: made};
