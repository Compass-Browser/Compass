// Run on chrome://compass-share/ (cdp.py PORT run URL --arg kind=K --arg id=UUID
// share.js [--arg role=editor|viewer]): shares the item if it isn't yet and
// returns a new invite link. docs/design/sync.md, "Rooms: clients".
const {ShareBrowserProxy} =
    await import('chrome://compass-share/share/browser_proxy.js');
const handler = ShareBrowserProxy.getInstance().handler;
const params = new URLSearchParams(location.search);
const kind = ARGS.kind || params.get('kind');
const id = ARGS.id || params.get('id');
let {item} = await handler.getItem(kind, id);
if (!item) {
  throw new Error('no such ' + kind);
}
let roomId = item.roomId;
if (!roomId) {
  const shared = await handler.share(kind, id);
  if (shared.error) {
    throw new Error(shared.error);
  }
  roomId = shared.roomId;
}
const made = await handler.createInviteLink(
    roomId, ARGS.role || 'editor', Number(ARGS.days || 7),
    Number(ARGS.uses || 0));
if (made.error) {
  throw new Error(made.error);
}
return {roomId, link: made.link};
