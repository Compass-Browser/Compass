// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome://compass-share: the share sheet (?kind=&id=), in a bubble, and the
// join page (#join=<an invite link's fragment>), in a tab.
// docs/design/sync.md, "Rooms: clients"; the wording matches iOS and
// Android.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import {applyBubbleLook} from '../bubble_look.js';
import {i18n, i18nF, i18nPlural} from '../i18n.js';
import type {Item, Member, Room} from '../share.mojom-webui.js';

import {ShareBrowserProxy} from './browser_proxy.js';

const proxy = ShareBrowserProxy.getInstance();
const app = document.querySelector<HTMLElement>('#app')!;
const title = document.querySelector<HTMLElement>('#title')!;

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

// The message |prefix| + Space, Easel or Folder, for |kind|, with |args|:
// each kind has its own sentence, as languages inflect around the noun.
function forKind(prefix: string, kind: string, ...args: string[]): string {
  switch (kind) {
    case 'space':
      return i18nF(prefix + 'Space', ...args);
    case 'easel':
      return i18nF(prefix + 'Easel', ...args);
    default:
      return i18nF(prefix + 'Folder', ...args);
  }
}

function roleName(role: string): string {
  switch (role) {
    case 'owner':
      return i18n('shareRoleOwner');
    case 'editor':
      return i18n('shareRoleEditor');
    default:
      return i18n('shareRoleViewer');
  }
}

function showError(where: HTMLElement, error: string) {
  where.textContent = error;
  where.hidden = !error;
}

// A captioned group: the caption, then a card for its rows.
function group(caption: string, ...rows: HTMLElement[]): HTMLElement {
  const card = el('div', {className: 'card'}, ...rows);
  app.append(el('div', {className: 'caption'}, caption), card);
  return card;
}

// Asks |question| under |row| in place of a dialog (a bubble has none):
// |action| runs on the confirming button. Asking again replaces the
// question.
function confirmInline(
    row: HTMLElement, question: string, action: string,
    onConfirm: () => void) {
  row.querySelector('.confirm')?.remove();
  const cancel = el('button', {}, i18n('cancel'));
  const yes = el('button', {className: 'danger primary'}, action);
  const box = el(
      'div', {className: 'confirm'}, el('p', {}, question),
      el('div', {className: 'row'}, cancel, yes));
  cancel.onclick = () => box.remove();
  yes.onclick = () => {
    box.remove();
    onConfirm();
  };
  row.append(box);
  yes.focus();
}

// The bubble shows once the sheet is drawn, at its size.
let shown = false;

function showUI() {
  if (!shown) {
    shown = true;
    proxy.handler.showUI();
  }
}

// The share sheet --------------------------------------------------------

let currentKind = '';
let currentId = '';
let currentRoomId = '';

async function renderSheet() {
  const {item} = await proxy.handler.getItem(currentKind, currentId);
  app.replaceChildren();
  if (!item) {
    title.textContent = i18n('shareTitle');
    app.append(el('p', {className: 'sub'}, i18n('shareItemGone')));
    return;
  }
  title.textContent =
      i18nF('shareTitleItem', item.name || i18n('shareUntitled'));
  if (!item.roomId) {
    renderNotShared(item);
    return;
  }
  currentRoomId = item.roomId;
  await renderRoom();
}

function renderNotShared(item: Item) {
  app.append(
      el('p', {className: 'sub'}, forKind('shareInvite', item.kind)));
  const error = el('p', {className: 'error', hidden: true});
  const share = el('button', {className: 'primary'}, i18n('shareStart'));
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
  app.append(el('div', {className: 'row actions'}, share), error);
  if (!item.canShare) {
    app.append(el('p', {className: 'note'}, i18n('shareNeedsSync')));
  }
}

async function renderRoom() {
  const {room, error} = await proxy.handler.getRoom(currentRoomId);
  // Read before the sheet is emptied, so the redraw happens at once.
  const {name} = await proxy.handler.getDisplayName();
  // Keep the scroll position across redraws.
  const scroll = app.scrollTop;
  app.replaceChildren();
  if (!room) {
    app.append(el('p', {className: 'error'}, error));
    return;
  }
  const owner = room.role === 'owner';
  app.append(el(
      'p', {className: 'sub'},
      i18nF('shareSharedOn', room.server) + ' ' +
          (room.role === 'viewer' ? i18n('shareViewOnly') :
                                    i18n('shareEncrypted'))));
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
  renderName(name);
  renderLeave(room);
  app.scrollTop = scroll;
}

function roleSelect(): HTMLSelectElement {
  const role = el('select', {});
  role.append(
      el('option', {value: 'editor'}, i18n('shareRoleEditor')),
      el('option', {value: 'viewer'}, i18n('shareRoleViewer')));
  return role;
}

function renderInviteLink(room: Room) {
  const role = roleSelect();
  const expiry = el('select', {});
  expiry.append(
      el('option', {value: '7'}, i18n('shareExpires7Days')),
      el('option', {value: '1'}, i18n('shareExpires1Day')),
      el('option', {value: '0'}, i18n('shareNeverExpires')));
  const uses = el('select', {});
  uses.append(
      el('option', {value: '0'}, i18n('shareUsesAny')),
      el('option', {value: '1'}, i18n('shareUsesOne')),
      el('option', {value: '5'}, i18nF('shareUsesUpTo', 5)),
      el('option', {value: '25'}, i18nF('shareUsesUpTo', 25)));
  const link = el('input', {
    type: 'text',
    readOnly: true,
    placeholder: i18n('shareNoLink'),
  });
  const copy =
      el('button', {className: 'primary'}, i18n('shareCopyInviteLink'));
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
    copy.textContent = i18n('shareCopied');
    setTimeout(() => copy.textContent = i18n('shareCopyInviteLink'), 2000);
  };
  group(
      i18n('shareInviteWithLink'),
      el('div', {className: 'row options'}, role, expiry, uses),
      el('div', {className: 'row'}, link, copy, error));
  app.append(el('p', {className: 'note'}, i18n('shareLinkNote')));
}

function renderEmailInvite(room: Room) {
  const email = el(
      'input', {type: 'email', placeholder: i18n('shareEmailPlaceholder')});
  const role = roleSelect();
  const invite = el('button', {}, i18n('shareInvite'));
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
  group(
      i18n('shareInviteByEmail'),
      el('div', {className: 'row'}, email, role, invite, error));
}

function memberName(member: Member): string {
  const name = member.name || member.email || i18n('shareSomeone');
  return member.isMe ? i18nF('shareMemberYou', name) : name;
}

function renderMembers(room: Room) {
  const owner = room.role === 'owner';
  const rows: HTMLElement[] = [];
  for (const member of room.members) {
    const who = el(
        'div', {className: 'who'},
        el('div', {},
           member.pending ? i18nF('shareMemberInvited', memberName(member)) :
                            memberName(member)));
    if (member.email && member.name) {
      who.append(el('div', {className: 'email'}, member.email));
    }
    const row = el('div', {className: 'row'}, who);
    if (owner && member.role !== 'owner') {
      const error = el('p', {className: 'error', hidden: true});
      const role = roleSelect();
      role.value = member.role;
      role.onchange = async () => {
        const result = await proxy.handler.changeRole(
            room.roomId, member.clientId, role.value);
        if (result.error) {
          role.value = member.role;
          showError(error, result.error);
          return;
        }
        await renderRoom();
      };
      const remove = el('button', {className: 'danger'}, i18n('remove'));
      remove.onclick = () => confirmInline(
          row,
          i18nF('shareRemoveMemberConfirm', memberName(member)),
          i18n('remove'), async () => {
            remove.disabled = true;
            const result =
                await proxy.handler.removeMember(room.roomId, member.clientId);
            if (result.error) {
              remove.disabled = false;
              showError(error, result.error);
              return;
            }
            await renderRoom();
          });
      row.append(role, remove, error);
    } else {
      row.append(el('span', {className: 'note'}, roleName(member.role)));
    }
    rows.push(row);
  }
  group(i18nF('sharePeople', room.members.length), ...rows);
}

function renderInvites(room: Room) {
  const open = room.invites.filter(
      i => !i.revoked && i.usesLeft !== 0 &&
          (!Number(i.expires) || Number(i.expires) > Date.now()));
  if (!open.length) {
    return;
  }
  const rows = open.map(invite => {
    const expires = Number(invite.expires) ?
        i18nF(
            'shareInviteExpires',
            new Date(Number(invite.expires)).toLocaleDateString()) :
        i18n('shareInviteNeverExpires');
    const uses = invite.usesLeft < 0 ?
        '' :
        ', ' + i18nPlural('shareInviteUsesLeft', invite.usesLeft);
    const revoke = el('button', {}, i18n('shareRevoke'));
    revoke.onclick = async () => {
      revoke.disabled = true;
      await proxy.handler.revokeInvite(room.roomId, invite.id);
      await renderRoom();
    };
    return el(
        'div', {className: 'row'},
        el('div', {className: 'who'},
           `${roleName(invite.role)} · ${expires}${uses}`),
        revoke);
  });
  group(i18n('shareInviteLinks'), ...rows);
}

function renderName(name: string) {
  const input = el('input', {type: 'text', value: name});
  input.onchange = () => proxy.handler.setDisplayName(input.value);
  group(i18n('shareYourName'), el('div', {className: 'row'}, input));
  app.append(el('p', {className: 'note'}, i18n('shareYourNameNote')));
}

function renderLeave(room: Room) {
  const owner = room.role === 'owner';
  const label =
      owner ? i18n('shareDeleteForEveryone') : i18n('shareLeave');
  const button = el('button', {className: 'danger'}, label);
  const error = el('p', {className: 'error', hidden: true});
  const row = el('div', {className: 'row actions'}, button);
  button.onclick = () => confirmInline(
      row,
      owner ? forKind('shareStopConfirm', room.kind) :
              forKind('shareLeaveConfirm', room.kind),
      label, async () => {
        button.disabled = true;
        const result = owner ? await proxy.handler.deleteRoom(room.roomId) :
                               await proxy.handler.leave(room.roomId);
        if (result.error) {
          showError(error, result.error);
          button.disabled = false;
          return;
        }
        proxy.handler.close();
      });
  app.append(row, error);
}

// The join page ----------------------------------------------------------

async function renderJoin(fragment: string) {
  const link = `https://compassbrowser.org/join#${fragment}`;
  const {preview} = await proxy.handler.previewJoin(link);
  app.replaceChildren();
  if (!preview) {
    title.textContent = i18n('shareBadLinkTitle');
    app.append(el('p', {className: 'sub'}, i18n('shareBadLink')));
    return;
  }
  title.textContent =
      i18nF('shareJoinTitle', preview.name || i18n('shareUntitled'));
  app.append(el(
      'p', {className: 'sub'},
      forKind('shareJoinInvited', preview.kind, preview.server)));
  if (preview.alreadyJoined) {
    app.append(el('p', {className: 'sub'}, i18n('shareAlreadyJoined')));
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
    group(
        i18n('sharePutItIn'),
        el('div', {className: 'row options'}, space));
  }
  const join = el('button', {className: 'primary'}, i18n('shareJoin'));
  const error = el('p', {className: 'error', hidden: true});
  join.onclick = async () => {
    join.disabled = true;
    join.textContent = i18n('shareJoining');
    const result = await proxy.handler.join(link, space.value || '');
    if (result.error) {
      showError(error, result.error);
      join.disabled = false;
      join.textContent = i18n('shareJoin');
      return;
    }
    title.textContent = i18n('shareJoined');
    app.replaceChildren(el(
        'p', {className: 'sub'},
        i18nF('shareJoinedInSidebar', preview.name)));
  };
  const {name} = await proxy.handler.getDisplayName();
  renderName(name);
  app.append(el('div', {className: 'row actions'}, join), error);
}

// ------------------------------------------------------------------------

// The colors: the browser's, as the bubble's frame has them.
async function applyLook() {
  const {look} = await proxy.handler.getLook();
  applyBubbleLook(look);
}

ColorChangeUpdater.forDocument().start();
window.matchMedia('(prefers-color-scheme: dark)')
    .addEventListener('change', applyLook);

// A change elsewhere (someone joined, a role changed) redraws the sheet,
// but never under the user's hands: an open menu, a half-typed field or a
// question would vanish. It waits until focus leaves them, and redraws are
// never stacked.
let refreshing = false;
let refreshWanted = false;

function busy(): boolean {
  const active = document.activeElement;
  return active instanceof HTMLSelectElement ||
      (active instanceof HTMLInputElement && !active.readOnly) ||
      !!app.querySelector('.confirm');
}

async function refresh() {
  if (!currentRoomId) {
    return;
  }
  if (refreshing || busy()) {
    refreshWanted = true;
    return;
  }
  refreshing = true;
  refreshWanted = false;
  try {
    await renderRoom();
  } finally {
    refreshing = false;
  }
  if (refreshWanted) {
    refresh();
  }
}

proxy.callbackRouter.roomsChanged.addListener(refresh);
document.addEventListener('focusout', () => {
  if (refreshWanted) {
    // After focus has landed on whatever comes next.
    setTimeout(refresh, 0);
  }
});
document.addEventListener('click', () => {
  if (refreshWanted) {
    // A question answered or dismissed.
    setTimeout(refresh, 0);
  }
});
document.querySelector<HTMLElement>('#close')!.onclick = () =>
    proxy.handler.close();

async function main() {
  const params = new URLSearchParams(location.search);
  const hash =
      location.hash.startsWith('#join=') ? location.hash.slice(6) : '';
  await applyLook();
  // The link stays out of the history and the address bar.
  if (hash) {
    document.documentElement.toggleAttribute('tab', true);
    history.replaceState(null, '', location.pathname);
    await renderJoin(hash);
  } else {
    currentKind = params.get('kind') || '';
    currentId = params.get('id') || '';
    await renderSheet();
  }
  showUI();
}

main();
