// Run on chrome://compass-share/ (cdp.py PORT run URL room.js --arg op=...):
//   op=join link=L [space=UUID]        joins; returns {error}
//   op=room room=ID                     the room as the share sheet sees it
//   op=role room=ID client=C role=R     changes a member's role
//   op=remove room=ID client=C          removes a member (and rotates)
//   op=leave room=ID / op=delete room=ID
//   op=name name=N                      "Your name for sharing"
const {ShareBrowserProxy} =
    await import('chrome://compass-share/share/browser_proxy.js');
const handler = ShareBrowserProxy.getInstance().handler;
// int64s come as BigInt, which can't be returned by value.
const plain = r => JSON.parse(JSON.stringify(
    r, (k, v) => typeof v === 'bigint' ? Number(v) : v));
switch (ARGS.op) {
  case 'join':
    return await handler.join(ARGS.link, ARGS.space || '');
  case 'room':
    return plain(await handler.getRoom(ARGS.room));
  case 'role':
    return await handler.changeRole(ARGS.room, ARGS.client, ARGS.role);
  case 'remove':
    return await handler.removeMember(ARGS.room, ARGS.client);
  case 'leave':
    return await handler.leave(ARGS.room);
  case 'delete':
    return await handler.deleteRoom(ARGS.room);
  case 'name':
    handler.setDisplayName(ARGS.name);
    return await handler.getDisplayName();
}
throw new Error('unknown op ' + ARGS.op);
