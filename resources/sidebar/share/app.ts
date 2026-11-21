// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome://compass-share: the share sheet (?kind=&id=) and the join page
// (#join=<an invite link's fragment>). docs/design/sync.md, "Rooms:
// clients"; the wording matches iOS and Android.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import type {Item, Member, Room} from '../share.mojom-webui.js';

import {ShareBrowserProxy} from './browser_proxy.js';

const proxy = ShareBrowserProxy.getInstance();
const app = document.querySelector<HTMLElement>('#app')!;

function el<K extends keyof HTMLElementTagNameMap>(
    tag: K, props: Partial<HTMLElementTagNameMap[K]> = {},
    ...children: Array<Node|string>): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  Object.assign(node, props);
  for (const child of children) {
    node.append(child);
  }
  return node;
}

function kindName(kind: string): string {
  switch (kind) {
    case 'space':
      return 'space';
    case 'easel':
      return 'easel';
    default:
      return 'folder';
  }
}

function roleName(role: string): string {
  switch (role) {
    case 'owner':
      return 'Owner';
    case 'editor':
      return 'Can edit';
    default:
      return 'Can view';
  }
}

function showError(where: HTMLElement, error: string) {
  where.textContent = error;
  where.hidden = !error;
}

// The share sheet --------------------------------------------------------

let currentKind = '';
let currentId = '';
let currentRoomId = '';

async function renderSheet() {
  const {item} = await proxy.handler.getItem(currentKind, currentId);
  app.replaceChildren();
  if (!item) {
    app.append(el('p', {className: 'sub'}, 'This item is no longer here.'));
    return;
  }
  app.append(el('h1', {}, `Share “${item.name || 'Untitled'}”`));
  if (!item.roomId) {
    renderNotShared(item);
    return;
  }
  currentRoomId = item.roomId;
  await renderRoom();
}

function renderNotShared(item: Item) {
  app.append(el(
      'p', {className: 'sub'},
      `Invite people to this ${kindName(item.kind)}. Everyone sees ` +
          'changes as they happen. It stays end-to-end encrypted: your ' +
          'sync server can’t read it.'));
  const error = el('p', {className: 'error', hidden: true});
  const share = el('button', {className: 'primary'}, 'Start sharing');
  share.disabled = !item.canShare;
  share.onclick = async () => {
    share.disabled = true;
    const {roomId, error: failed} =
        await proxy.handler.share(item.kind, item.id);
    if (failed) {
      showError(error, failed);
      share.disabled = false;
      return;
    }
    currentRoomId = roomId;
    await renderSheet();
  };
  app.append(el('div', {className: 'row'}, share), error);
  if (!item.canShare) {
    app.append(el(
        'p', {className: 'note'},
        'Turn on sync to share. Sharing goes through your sync server.'));
  }
}

async function renderRoom() {
  const {room, error} = await proxy.handler.getRoom(currentRoomId);
  // Keep the title.
  const title = app.querySelector('h1');
  app.replaceChildren();
  if (title) {
    app.append(title);
  }
  if (!room) {
    app.append(el('p', {className: 'error'}, error));
    return;
  }
  const owner = room.role === 'owner';
  app.append(el(
      'p', {className: 'sub'},
      `Shared on ${room.server}. ` +
          (room.role === 'viewer' ? 'You can view it.' :
                                    'End-to-end encrypted.')));
  if (owner) {
    renderInviteLink(room);
    if (room.oidc) {
      renderEmailInvite(room);
    }
  }
  renderMembers(room);
  if (owner) {
    renderInvites(room);
  }
  await renderName();
  renderLeave(room);
}

function renderInviteLink(room: Room) {
  app.append(el('h2', {}, 'Invite with a link'));
  const role = el('select', {});
  role.append(
      el('option', {value: 'editor'}, 'Can edit'),
      el('option', {value: 'viewer'}, 'Can view'));
  const expiry = el('select', {});
  expiry.append(
      el('option', {value: '7'}, 'Expires in 7 days'),
      el('option', {value: '1'}, 'Expires in 1 day'),
      el('option', {value: '0'}, 'Never expires'));
  const uses = el('select', {});
  uses.append(
      el('option', {value: '0'}, 'Any number of people'),
      el('option', {value: '1'}, 'One person'),
      el('option', {value: '5'}, 'Up to 5 people'),
      el('option', {value: '25'}, 'Up to 25 people'));
  const link = el('input', {
    type: 'text',
    className: 'link',
    readOnly: true,
    placeholder: 'No link yet',
  });
  const copy = el('button', {className: 'primary'}, 'Copy invite link');
  const error = el('p', {className: 'error', hidden: true});
  copy.onclick = async () => {
    copy.disabled = true;
    const result = await proxy.handler.createInviteLink(
        room.roomId, role.value, Number(expiry.value), Number(uses.value));
    copy.disabled = false;
    if (result.error) {
      showError(error, result.error);
      return;
    }
    showError(error, '');
    link.value = result.link;
    proxy.handler.copyToClipboard(result.link);
    copy.textContent = 'Copied';
    setTimeout(() => copy.textContent = 'Copy invite link', 2000);
  };
  app.append(
      el('div', {className: 'row'}, role, expiry, uses),
      el('div', {className: 'row'}, link, copy), error,
      el('p', {className: 'note'},
         'Anyone with the link can join until it expires or you revoke it.'));
}

function renderEmailInvite(room: Room) {
  app.append(el('h2', {}, 'Invite by email'));
  const email = el('input', {type: 'email', placeholder: 'name@company.com'});
  const role = el('select', {});
  role.append(
      el('option', {value: 'editor'}, 'Can edit'),
      el('option', {value: 'viewer'}, 'Can view'));
  const invite = el('button', {}, 'Invite');
  const error = el('p', {className: 'error', hidden: true});
  invite.onclick = async () => {
    invite.disabled = true;
    const result =
        await proxy.handler.inviteByEmail(room.roomId, email.value, role.value);
    invite.disabled = false;
    showError(error, result.error);
    if (!result.error) {
      email.value = '';
      await renderRoom();
    }
  };
  app.append(el('div', {className: 'row'}, email, role, invite), error);
}

function memberName(member: Member): string {
  const name = member.name || member.email || 'Someone';
  return member.isMe ? `${name} (you)` : name;
}

function renderMembers(room: Room) {
  app.append(el('h2', {}, `People (${room.members.length})`));
  const owner = room.role === 'owner';
  for (const member of room.members) {
    const who = el(
        'div', {className: 'who'},
        el('div', {}, memberName(member) + (member.pending ? ' · invited' :
                                                             '')));
    if (member.email && member.name) {
      who.append(el('div', {className: 'email'}, member.email));
    }
    const row = el('div', {className: 'row member'}, who);
    if (owner && member.role !== 'owner') {
      const role = el('select', {});
      role.append(
          el('option', {value: 'editor'}, 'Can edit'),
          el('option', {value: 'viewer'}, 'Can view'));
      role.value = member.role;
      role.onchange = async () => {
        const {error} = await proxy.handler.changeRole(
            room.roomId, member.clientId, role.value);
        if (error) {
          alert(error);
        }
        await renderRoom();
      };
      const remove = el('button', {className: 'danger'}, 'Remove');
      remove.onclick = async () => {
        if (!confirm(`Remove ${memberName(member)}? They keep a copy of ` +
                     'what was shared, but won’t see new changes.')) {
          return;
        }
        remove.disabled = true;
        const {error} =
            await proxy.handler.removeMember(room.roomId, member.clientId);
        if (error) {
          alert(error);
        }
        await renderRoom();
      };
      row.append(role, remove);
    } else {
      row.append(el('span', {className: 'note'}, roleName(member.role)));
    }
    app.append(row);
  }
}

function renderInvites(room: Room) {
  const open = room.invites.filter(
      i => !i.revoked && i.usesLeft !== 0 &&
          (!Number(i.expires) || Number(i.expires) > Date.now()));
  if (!open.length) {
    return;
  }
  app.append(el('h2', {}, 'Invite links'));
  for (const invite of open) {
    const expires = Number(invite.expires) ?
        `expires ${new Date(Number(invite.expires)).toLocaleDateString()}` :
        'never expires';
    const uses = invite.usesLeft < 0 ? '' : `, ${invite.usesLeft} uses left`;
    const revoke = el('button', {}, 'Revoke');
    revoke.onclick = async () => {
      revoke.disabled = true;
      await proxy.handler.revokeInvite(room.roomId, invite.id);
      await renderRoom();
    };
    app.append(el(
        'div', {className: 'row member'},
        el('div', {className: 'who'},
           `${roleName(invite.role)} · ${expires}${uses}`),
        revoke));
  }
}

async function renderName() {
  app.append(el('h2', {}, 'Your name for sharing'));
  const {name} = await proxy.handler.getDisplayName();
  const input = el('input', {type: 'text', value: name});
  input.onchange = () => proxy.handler.setDisplayName(input.value);
  app.append(
      el('div', {className: 'row'}, input),
      el('p', {className: 'note'},
         'What the people you share with see, unless your company’s ' +
             'sign-in gives your name.'));
}

function renderLeave(room: Room) {
  const owner = room.role === 'owner';
  const button = el(
      'button', {className: 'danger'},
      owner ? 'Delete for everyone' : 'Leave');
  const error = el('p', {className: 'error', hidden: true});
  button.onclick = async () => {
    const question = owner ?
        `Stop sharing this ${kindName(room.kind)} and remove it from ` +
            'everyone else? You keep it.' :
        `Leave this ${kindName(room.kind)}? You keep a copy, but won’t ` +
            'see new changes.';
    if (!confirm(question)) {
      return;
    }
    button.disabled = true;
    const result = owner ? await proxy.handler.deleteRoom(room.roomId) :
                           await proxy.handler.leave(room.roomId);
    if (result.error) {
      showError(error, result.error);
      button.disabled = false;
      return;
    }
    proxy.handler.close();
  };
  app.append(el('h2', {}, ''), el('div', {className: 'row'}, button), error);
}

// The join page ----------------------------------------------------------

async function renderJoin(fragment: string) {
  const link = `https://compassbrowser.org/join#${fragment}`;
  const {preview} = await proxy.handler.previewJoin(link);
  app.replaceChildren();
  if (!preview) {
    app.append(
        el('h1', {}, 'This invite link doesn’t work'),
        el('p', {className: 'sub'},
           'It may have been copied only in part. Ask for a new one.'));
    return;
  }
  const kind = kindName(preview.kind);
  app.append(
      el('h1', {}, `Join “${preview.name || 'Untitled'}”`),
      el('p', {className: 'sub'},
         `You’ve been invited to a shared ${kind} on ` +
             `${preview.server}. The person who shared it decides whether ` +
             'you can edit it or only view it.'));
  if (preview.alreadyJoined) {
    app.append(el('p', {}, 'You’ve already joined it.'));
    return;
  }
  const space = el('select', {});
  if (preview.kind !== 'space') {
    const {spaces} = await proxy.handler.getSpaces();
    for (const option of spaces) {
      const node = el('option', {value: option.id}, option.name);
      node.selected = option.active;
      space.append(node);
    }
    app.append(el(
        'div', {className: 'row'}, el('span', {}, 'Put it in'), space));
  }
  const join = el('button', {className: 'primary'}, 'Join');
  const error = el('p', {className: 'error', hidden: true});
  join.onclick = async () => {
    join.disabled = true;
    join.textContent = 'Joining…';
    const result = await proxy.handler.join(link, space.value || '');
    if (result.error) {
      showError(error, result.error);
      join.disabled = false;
      join.textContent = 'Join';
      return;
    }
    app.replaceChildren(
        el('h1', {}, 'Joined'),
        el('p', {className: 'sub'},
           `“${preview.name}” is in your sidebar.`));
  };
  app.append(el('div', {className: 'row'}, join), error);
  await renderName();
}

// ------------------------------------------------------------------------

ColorChangeUpdater.forDocument().start();
proxy.callbackRouter.roomsChanged.addListener(() => {
  if (currentRoomId) {
    renderRoom();
  }
});

const params = new URLSearchParams(location.search);
const hash = location.hash.startsWith('#join=') ? location.hash.slice(6) : '';
// The link stays out of the history and the address bar.
if (hash) {
  history.replaceState(null, '', location.pathname);
  renderJoin(hash);
} else {
  currentKind = params.get('kind') || '';
  currentId = params.get('id') || '';
  renderSheet();
}
