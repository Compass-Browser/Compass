// Run in a window's sidebar: cdp.py PORT in compass-sidebar profiles.js
// --arg op=OP [--arg ...]. Profiles and the spaces of each, as the switcher
// shows them (tools/sync_e2e/README.md).
//
//   op=dump                         {order, profiles: {NAME: {dir, spaces:
//                                   [{name, pinned}], favorites}}}
//   op=assign space=S [profile=DIR] assigns space S to the profile in
//                                   directory DIR ("Default", "Profile 1"),
//                                   or to a new profile without one
const {SidebarBrowserProxy} =
    await import('chrome://compass-sidebar/browser_proxy.js');
const proxy = SidebarBrowserProxy.getInstance();
const handler = proxy.handler;
const router = proxy.callbackRouter;
const sleep = ms => new Promise(r => setTimeout(r, ms));

// url.mojom.Url is a plain string in these bindings.
function urlOf(node) {
  if (!node.url) return '';
  return typeof node.url === 'string' ? node.url : (node.url.url || '');
}

function plain(node) {
  const out = {type: ['root', 'folder', 'tab', 'split'][node.type]};
  if (node.title) out.title = node.title;
  const url = urlOf(node);
  if (url) out.url = url;
  if (node.children.length) out.children = node.children.map(plain);
  return out;
}

// The switcher's spaces, the window's active one, and the profiles.
function spaces() {
  return new Promise(resolve => {
    const id = router.setSpaces.addListener((list, activeId, profiles) => {
      router.removeListener(id);
      resolve({list, activeId, profiles});
    });
    handler.requestTree();
  });
}

switch (ARGS.op) {
  case 'dump': {
    const {list, profiles} = await spaces();
    const result = {order: list.map(s => s.name), profiles: {}};
    for (const space of list) {
      const {tree} = await handler.getSpaceTree(space.id);
      const name = space.profileName || space.profileId;
      const profile = result.profiles[name] ||
          (result.profiles[name] = {dir: space.profileId, spaces: []});
      profile.spaces.push({
        name: space.name,
        pinned: tree ? (tree.pinned.children || []).map(plain) : null,
      });
      if (tree) {
        profile.favorites = (tree.favorites.children || []).map(plain);
      }
    }
    result.options = profiles.map(p => ({dir: p.id, name: p.name}));
    return result;
  }
  case 'assign': {
    let {list, activeId} = await spaces();
    const space = list.find(s => s.name === ARGS.space);
    if (!space) {
      throw new Error('no space ' + ARGS.space + ' in ' +
                      JSON.stringify(list.map(s => s.name)));
    }
    if (space.id === activeId) {
      // A shown space's window would swap and close this page: show
      // another space of the window's profile first.
      const other = list.find(s => s.ownProfile && s.id !== space.id);
      if (!other) {
        throw new Error('show another space of this profile first');
      }
      handler.switchSpace(other.id);
      for (let i = 0; i < 40 && activeId !== other.id; ++i) {
        await sleep(250);
        ({activeId} = await spaces());
      }
    }
    const before = space.profileId;
    handler.setSpaceProfile(space.id, ARGS.profile || null);
    // A new profile takes a few seconds to load.
    for (let i = 0; i < 240; ++i) {
      await sleep(250);
      ({list} = await spaces());
      const now = list.find(s => s.name === ARGS.space);
      if (now && now.profileId !== before) {
        return {space: now.name, profile: now.profileId,
                profileName: now.profileName};
      }
    }
    throw new Error('the space stayed in ' + before);
  }
  default:
    throw new Error('unknown op ' + ARGS.op);
}
