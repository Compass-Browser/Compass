// Run in a window's sidebar: cdp.py PORT in compass-sidebar sidebar.js
// --arg op=OP [--arg ...]. Drives the sidebar's page handler, as the user
// would (docs/design/sync-slice-3-steps.md). Nodes are named by title,
// custom title or URL; "pinned" is the space's Pinned root, "favorites" the
// favorites root. Spaces are named by name.
//
//   op=dump                                   spaces and their trees, as JSON
//   op=add-space name=N color=blue            adds a space, names and colors it
//   op=rename-space space=S name=N
//   op=move-space space=S index=I             in the switcher
//   op=delete-space space=S
//   op=add-folder space=S title=T [parent=P]  at the end of P (default pinned)
//   op=open space=S url=U [parent=P]          a new entry for U at the end of P
//   op=rename space=S node=X title=T          a folder's name or a tab's
//   op=move space=S node=X parent=P [index=I] (default: the end)
//   op=collapse space=S node=X [collapsed=false]
//   op=delete space=S node=X
const {SidebarBrowserProxy} =
    await import('chrome://compass-sidebar/browser_proxy.js');
const proxy = SidebarBrowserProxy.getInstance();
const handler = proxy.handler;
const router = proxy.callbackRouter;
const COLORS =
    ['grey', 'blue', 'purple', 'pink', 'red', 'orange', 'yellow', 'green'];
const sleep = ms => new Promise(r => setTimeout(r, ms));

// The switcher's spaces and the window's active one.
function spaces() {
  return new Promise(resolve => {
    const id = router.setSpaces.addListener((list, activeId) => {
      router.removeListener(id);
      resolve({list, activeId});
    });
    handler.requestTree();
  });
}

async function findSpace(name) {
  const {list} = await spaces();
  const space = list.find(s => s.name === name);
  if (!space) {
    throw new Error('no space ' + name + ' in ' +
                    JSON.stringify(list.map(s => s.name)));
  }
  return space;
}

// Shows |name| in this window (the handler acts on the window's space).
async function show(name) {
  const space = await findSpace(name);
  for (let i = 0; i < 40; ++i) {
    const {activeId} = await spaces();
    if (activeId === space.id) {
      return space;
    }
    if (i === 0) {
      handler.switchSpace(space.id);
    }
    await sleep(250);
  }
  throw new Error('could not switch to ' + name);
}

function plain(node) {
  const out = {type: ['root', 'folder', 'tab', 'split'][node.type]};
  if (node.title) out.title = node.title;
  if (node.customTitle) out.customTitle = node.customTitle;
  if (node.url && node.url.url) out.url = node.url.url;
  if (node.collapsed) out.collapsed = true;
  if (node.children.length) out.children = node.children.map(plain);
  return out;
}

async function tree(spaceId) {
  const {tree: result} = await handler.getSpaceTree(spaceId);
  if (!result) {
    throw new Error('no tree for ' + spaceId);
  }
  return result;
}

function find(node, match) {
  if (node.title === match || node.customTitle === match ||
      (node.url && node.url.url === match)) {
    return node;
  }
  for (const child of node.children) {
    const found = find(child, match);
    if (found) {
      return found;
    }
  }
  return null;
}

async function resolve(space, match) {
  const t = await tree(space.id);
  if (!match || match === 'pinned') {
    return t.pinned;
  }
  if (match === 'favorites') {
    return t.favorites;
  }
  const node = find(t.pinned, match) || find(t.favorites, match);
  if (!node) {
    throw new Error('no node ' + match + ' in ' + space.name);
  }
  return node;
}

// Waits for the handler's work to reach the model and come back.
async function settle() {
  await sleep(500);
  await spaces();
}

switch (ARGS.op) {
  case 'dump': {
    const {list, activeId} = await spaces();
    const result = {active: activeId, spaces: []};
    for (const space of list) {
      const t = await tree(space.id);
      result.spaces.push({
        id: space.id,
        name: space.name,
        color: COLORS[space.color],
        archiveAfterHours: space.archiveAfterHours,
        pinned: (t.pinned.children || []).map(plain),
      });
      result.favorites = (t.favorites.children || []).map(plain);
    }
    return result;
  }
  case 'add-space': {
    const before = new Set((await spaces()).list.map(s => s.id));
    handler.addSpace();
    for (let i = 0; i < 40; ++i) {
      await sleep(250);
      const added = (await spaces()).list.find(s => !before.has(s.id));
      if (added) {
        handler.renameSpace(added.id, ARGS.name);
        if (ARGS.color) {
          handler.setSpaceColor(added.id, COLORS.indexOf(ARGS.color));
        }
        await settle();
        return {added: added.id};
      }
    }
    throw new Error('no space added');
  }
  case 'rename-space': {
    const space = await findSpace(ARGS.space);
    handler.renameSpace(space.id, ARGS.name);
    await settle();
    return 'renamed';
  }
  case 'move-space': {
    const space = await findSpace(ARGS.space);
    handler.moveSpace(space.id, Number(ARGS.index));
    await settle();
    return 'moved';
  }
  case 'delete-space': {
    const space = await findSpace(ARGS.space);
    handler.deleteSpace(space.id);
    await settle();
    return 'deleted';
  }
  case 'add-folder': {
    const space = await show(ARGS.space);
    const parent = await resolve(space, ARGS.parent);
    handler.addFolder(parent.id, parent.children.length, ARGS.title);
    await settle();
    return 'added';
  }
  case 'open': {
    const space = await show(ARGS.space);
    const parent = await resolve(space, ARGS.parent);
    handler.openLink({url: ARGS.url}, parent.id, parent.children.length);
    await settle();
    return 'opened';
  }
  case 'rename': {
    const space = await show(ARGS.space);
    const node = await resolve(space, ARGS.node);
    if (node.type === 1) {
      handler.renameFolder(node.id, ARGS.title);
    } else {
      handler.renameTab(node.id, ARGS.title);
    }
    await settle();
    return 'renamed';
  }
  case 'move': {
    const space = await show(ARGS.space);
    const node = await resolve(space, ARGS.node);
    const parent = await resolve(space, ARGS.parent);
    const index =
        ARGS.index !== undefined ? Number(ARGS.index) : parent.children.length;
    handler.moveNode(node.id, parent.id, index);
    await settle();
    return 'moved';
  }
  case 'collapse': {
    const space = await show(ARGS.space);
    const node = await resolve(space, ARGS.node);
    handler.setFolderCollapsed(node.id, ARGS.collapsed !== 'false');
    await settle();
    return 'collapsed';
  }
  case 'delete': {
    const space = await show(ARGS.space);
    const node = await resolve(space, ARGS.node);
    handler.deleteNode(node.id);
    await settle();
    return 'deleted';
  }
  default:
    throw new Error('unknown op ' + ARGS.op);
}
