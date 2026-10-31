// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import type {Pairing, Status} from './compass_sync.mojom-webui.js';
import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote, PairingStep, State} from './compass_sync.mojom-webui.js';

// chrome://compass-sync: slice 2's minimal sync page (docs/design/sync.md,
// "UI"). Off: start a chain, or join one by pairing code or recovery code.
// On: add a device, leave.
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

// Set while the recovery code is on screen; the page shows nothing else
// until "I saved it".
let showingWords = false;

function render(status: Status, pairing: Pairing) {
  if (showingWords) {
    return;
  }
  $('status').textContent = status.message;
  const state = status.state;
  if (status.serverUrl && !value('server')) {
    $<HTMLInputElement>('server').value = status.serverUrl;
  }
  const pairingActive = pairing.step !== PairingStep.kNone;
  show('off', state === State.kOff);
  show('restartPanel', state === State.kNeedsRestart);
  show('on',
       (state === State.kOn || state === State.kStarting ||
        state === State.kError) &&
           !pairingActive);
  $<HTMLButtonElement>('addDevice').disabled = state !== State.kOn;

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
  const {status, pairing} = await handler.getState();
  render(status, pairing);
}

callbackRouter.onStateChanged.addListener(render);

$('start').addEventListener('click', async () => {
  $('offError').textContent = '';
  const {ok, error, words} = await handler.startSync(value('server'));
  if (!ok) {
    $('offError').textContent = error;
    return;
  }
  const list = $('words');
  list.replaceChildren(...words.map(word => {
    const item = document.createElement('li');
    item.textContent = word;
    return item;
  }));
  showingWords = true;
  show('off', false);
  show('wordsPanel', true);
});

$('saved').addEventListener('click', () => {
  $('words').replaceChildren();
  showingWords = false;
  show('wordsPanel', false);
  refresh();
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

$('leave').addEventListener('click', () => {
  if (window.confirm(
          'Leave sync? This device keeps its data but stops syncing. ' +
          'Compass restarts to finish.')) {
    handler.leave();
  }
});

refresh();
