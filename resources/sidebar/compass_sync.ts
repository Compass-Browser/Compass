// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import type {DataTypeState, Details, Device, Pairing, Status} from './compass_sync.mojom-webui.js';
import {DataType, PageCallbackRouter, PageHandlerFactory, PageHandlerRemote, PairingStep, State} from './compass_sync.mojom-webui.js';

// chrome://compass-sync (docs/design/sync.md, "UI" and "Slice 4 notes").
// Off: start a chain on a server, or join one by pairing code or recovery
// code. On: the status, the devices, adding one, what syncs, the recovery
// code (behind Touch ID), the server, leaving and resetting.
const callbackRouter = new PageCallbackRouter();
const handler = new PageHandlerRemote();
PageHandlerFactory.getRemote().createPageHandler(
    callbackRouter.$.bindNewPipeAndPassRemote(),
    handler.$.bindNewPipeAndPassReceiver());

ColorChangeUpdater.forDocument().start();

function $<T extends HTMLElement>(id: string): T {
  return document.getElementById(id) as T;
}

function show(id: string, visible: boolean) {
  $(id).hidden = !visible;
}

function value(id: string): string {
  return $<HTMLInputElement|HTMLTextAreaElement>(id).value.trim();
}

// The switches, in the page's order: name and what they cover.
const TYPE_LABELS = new Map<DataType, [string, string]>([
  [DataType.kSpaces, ['Spaces', 'Spaces, pinned tabs, folders, favorites']],
  [DataType.kPasswords, ['Passwords', 'Saved passwords and passkeys']],
  [DataType.kHistory, ['History', 'Pages you visited']],
  [DataType.kBookmarks, ['Bookmarks', '']],
  [DataType.kAutofill, ['Autofill', 'Addresses and form entries']],
  [DataType.kExtensions, ['Extensions', 'Extensions and their settings']],
  [DataType.kSettings, ['Settings', 'Settings and search engines']],
  [DataType.kReadingList, ['Reading list', '']],
  [DataType.kOpenTabs, ['Open tabs', 'Your tabs, for the other devices']],
]);

// Set while a recovery code is on screen: the page shows nothing else until
// it's dismissed.
let showingWords = false;
let latest: {status: Status, pairing: Pairing, details: Details}|null = null;

function stateName(state: State): string {
  switch (state) {
    case State.kOn:
      return 'on';
    case State.kStarting:
      return 'starting';
    case State.kNeedsRestart:
      return 'restart';
    case State.kError:
      return 'error';
    default:
      return 'off';
  }
}

// "Active now", "5 minutes ago", "yesterday"...
function lastSeen(ms: number): string {
  if (!ms) {
    return '';
  }
  const seconds = Math.max(0, (Date.now() - ms) / 1000);
  if (seconds < 120) {
    return 'active now';
  }
  const format = new Intl.RelativeTimeFormat(undefined, {numeric: 'auto'});
  if (seconds < 3600) {
    return format.format(-Math.round(seconds / 60), 'minute');
  }
  if (seconds < 86400) {
    return format.format(-Math.round(seconds / 3600), 'hour');
  }
  return format.format(-Math.round(seconds / 86400), 'day');
}

function renderDevices(devices: Device[]) {
  $('devices').replaceChildren(...devices.map(device => {
    const item = document.createElement('li');
    const text = document.createElement('div');
    text.className = 'grow';
    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = device.name || 'Unnamed device';
    const details = document.createElement('div');
    details.className = 'details';
    details.textContent =
        [device.kind, lastSeen(device.lastSeenMs)].filter(s => s).join(' · ');
    text.append(name, details);
    item.append(text);
    if (device.thisDevice) {
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = 'This device';
      item.append(badge);
    }
    item.dataset['thisDevice'] = String(device.thisDevice);
    return item;
  }));
  show('devices', devices.length > 0);
  show('noDevices', devices.length === 0);
}

function renderTypes(types: DataTypeState[]) {
  const enabled = new Map(types.map(t => [t.type, t.enabled]));
  const list = $('types');
  // Built once; later only the switches' states change (a rebuild would
  // drop focus from a switch the user just toggled).
  if (!list.children.length) {
    for (const [type, [label, description]] of TYPE_LABELS) {
      const item = document.createElement('li');
      const text = document.createElement('label');
      text.className = 'grow';
      text.htmlFor = `type-${type}`;
      const name = document.createElement('div');
      name.className = 'name';
      name.textContent = label;
      text.append(name);
      if (description) {
        const details = document.createElement('div');
        details.className = 'details';
        details.textContent = description;
        text.append(details);
      }
      const toggle = document.createElement('input');
      toggle.type = 'checkbox';
      toggle.className = 'switch';
      toggle.id = `type-${type}`;
      toggle.setAttribute('role', 'switch');
      toggle.dataset['type'] = String(type);
      toggle.addEventListener(
          'change', () => handler.setDataTypeEnabled(type, toggle.checked));
      item.append(text, toggle);
      list.append(item);
    }
  }
  for (const toggle of list.querySelectorAll<HTMLInputElement>(
           'input.switch')) {
    const type = Number(toggle.dataset['type']) as DataType;
    toggle.checked = enabled.get(type) ?? true;
  }
}

function render(status: Status, pairing: Pairing, details: Details) {
  latest = {status, pairing, details};
  // For the end-to-end steps: the app menu's row for this state.
  document.documentElement.dataset['menuLabel'] = status.menuLabel;
  $('statusDot').dataset['state'] = stateName(status.state);
  $('status').textContent = status.message ||
      (status.state === State.kOff ? 'Sync is off.' : '');
  if (showingWords) {
    return;
  }
  const state = status.state;
  const pairingActive = pairing.step !== PairingStep.kNone;
  const on = state === State.kOn || state === State.kStarting ||
      state === State.kError;
  show('off', state === State.kOff);
  show('restartPanel', state === State.kNeedsRestart);
  show('on', on && !pairingActive);
  $<HTMLButtonElement>('addDevice').disabled = state !== State.kOn;
  $('serverUrl').textContent = status.serverUrl;
  renderDevices(details.devices);
  renderTypes(details.types);

  show('pairing', pairingActive);
  $('pairingCode').textContent = pairing.code;
  $('pairingMessage').textContent = pairing.message;
  $('pairingMessage').classList.toggle(
      'error', pairing.step === PairingStep.kFailed);
  show('confirm', pairing.step === PairingStep.kConfirm);
  $('deviceName').textContent = pairing.deviceName;
  $('cancelPairing').textContent =
      pairing.step === PairingStep.kDone || pairing.step === PairingStep.kFailed ?
      'Close' :
      'Cancel';
  if (pairing.step === PairingStep.kDone && state === State.kNeedsRestart) {
    show('restartPanel', true);
  }
}

async function refresh() {
  const {status, pairing, details} = await handler.getState();
  render(status, pairing, details);
}

callbackRouter.onStateChanged.addListener(render);

// Shows |words| alone on the page until the user dismisses them.
function showWords(words: string[], dismiss: string) {
  $('words').replaceChildren(...words.map(word => {
    const item = document.createElement('li');
    item.textContent = word;
    return item;
  }));
  $('saved').textContent = dismiss;
  showingWords = true;
  for (const id of ['off', 'on', 'pairing', 'restartPanel']) {
    show(id, false);
  }
  show('wordsPanel', true);
}

$('saved').addEventListener('click', () => {
  $('words').replaceChildren();
  showingWords = false;
  show('wordsPanel', false);
  if (latest) {
    render(latest.status, latest.pairing, latest.details);
  }
  refresh();
});

// A modal question; resolves to whether the user confirmed.
function confirmAction(title: string, text: string, ok: string):
    Promise<boolean> {
  const dialog = $<HTMLDialogElement>('confirmDialog');
  $('confirmTitle').textContent = title;
  $('confirmText').textContent = text;
  $('confirmOk').textContent = ok;
  $('confirmOk').className = ok === 'Reset sync' ? 'danger' : '';
  return new Promise(resolve => {
    const done = (result: boolean) => {
      $('confirmOk').onclick = null;
      $('confirmCancel').onclick = null;
      dialog.onclose = null;
      dialog.close();
      resolve(result);
    };
    $('confirmOk').onclick = () => done(true);
    $('confirmCancel').onclick = () => done(false);
    dialog.onclose = () => resolve(false);
    dialog.showModal();
  });
}

$('start').addEventListener('click', async () => {
  $('offError').textContent = '';
  const {ok, error, words} = await handler.startSync(value('server'));
  if (!ok) {
    $('offError').textContent = error;
    return;
  }
  showWords(words, 'I saved it');
});

$('showJoin').addEventListener('click', () => {
  show('joinPanel', true);
  $('code').focus();
});

$('join').addEventListener('click', () => {
  $('offError').textContent = '';
  handler.joinWithPairingCode(value('server'), value('code'));
});

$('showRecovery').addEventListener('click', () => {
  show('recoveryPanel', true);
  $('recovery').focus();
});

$('recover').addEventListener('click', async () => {
  $('offError').textContent = '';
  const {ok, error} =
      await handler.joinWithRecoveryCode(value('server'), value('recovery'));
  if (!ok) {
    $('offError').textContent = error;
    return;
  }
  $<HTMLTextAreaElement>('recovery').value = '';
  refresh();
});

$('addDevice').addEventListener('click', () => handler.addDevice());
$('allow').addEventListener('click', () => handler.answerPairing(true));
$('deny').addEventListener('click', () => handler.answerPairing(false));
$('cancelPairing').addEventListener('click', () => handler.cancelPairing());
$('restart').addEventListener('click', () => handler.restart());

$('showWords').addEventListener('click', async () => {
  $('wordsError').textContent = '';
  const {ok, error, words} = await handler.revealRecoveryCode();
  if (!ok) {
    // Empty when the user canceled Touch ID.
    $('wordsError').textContent = error;
    return;
  }
  showWords(words, 'Hide');
});

$('leave').addEventListener('click', async () => {
  if (await confirmAction(
          'Leave sync?',
          'This device keeps its data but stops syncing. Your other ' +
              'devices go on syncing. Compass restarts to finish.',
          'Leave sync')) {
    handler.leave();
  }
});

$('reset').addEventListener('click', async () => {
  $('onError').textContent = '';
  if (!await confirmAction(
          'Reset sync?',
          'Compass makes a new sync chain with a new recovery code, and ' +
              'this device\'s data goes up to it after a restart. Your ' +
              'other devices stay on the old chain until you add them ' +
              'again, and the old recovery code stops working here.',
          'Reset sync')) {
    return;
  }
  const {ok, error, words} = await handler.resetSync();
  if (!ok) {
    $('onError').textContent = error;
    return;
  }
  showWords(words, 'I saved it');
});

refresh();
