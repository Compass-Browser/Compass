// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {DataTypeState, Details, Device, Pairing, ProfileMapping, ProfileMappingEntry, Status} from '../compass_sync.mojom-webui.js';
import {DataType, PageCallbackRouter, PageHandlerFactory, PageHandlerRemote, PairingStep, State} from '../compass_sync.mojom-webui.js';

import {CompassSettingsPage} from './page_base.js';
import {getTemplate} from './sync_page.html.js';

// Sync: compass://settings/sync, and alone on chrome://compass-sync
// (docs/design/sync.md, "UI" and "Slice 4 notes"). Off: start a chain on a
// server, or join one by pairing code or recovery code. On: the status, the
// devices, adding one, what syncs, the recovery code (behind Touch ID), the
// server, leaving and resetting. Joining on a Mac with other profiles: which
// of the chain's each one syncs with (slice 5).

// The switches, in the page's order: name and what they cover. Bookmarks
// have none: Compass has no bookmarks (ARCHITECTURE.md D20), so the type
// keeps its setting out of sight.
const TYPE_LABELS = new Map<DataType, [string, string]>([
  [DataType.kSpaces, ['Spaces', 'Spaces, pinned tabs, folders, favorites']],
  [DataType.kPasswords, ['Passwords', 'Saved passwords and passkeys']],
  [DataType.kHistory, ['History', 'Pages you visited']],
  [DataType.kAutofill, ['Autofill', 'Addresses and form entries']],
  [DataType.kExtensions, ['Extensions', 'Extensions and their settings']],
  [DataType.kSettings, ['Settings', 'Settings and search engines']],
  [DataType.kReadingList, ['Reading list', '']],
  [DataType.kOpenTabs, ['Open tabs', 'Your tabs, for the other devices']],
]);

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

export class CompassSyncPageElement extends CompassSettingsPage {
  private callbackRouter_ = new PageCallbackRouter();
  private handler_ = new PageHandlerRemote();
  // Set while a recovery code is on screen: the page shows nothing else
  // until it's dismissed.
  private showingWords_ = false;
  private latest_: {status: Status, pairing: Pairing, details: Details}|null =
      null;
  // The profile mapping the rows show (slice 5), so a status change doesn't
  // reset the user's choices.
  private mappingShown_ = '';
  // Ticks the "Expires in m:ss" line while a code shows.
  private expiryTimer_: number|null = null;
  // The invite whose QR code is drawn, so a status change doesn't redraw it.
  private qrShown_ = '';

  protected get pageTitle() {
    return 'Sync';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter_.$.bindNewPipeAndPassRemote(),
        this.handler_.$.bindNewPipeAndPassReceiver());
    this.callbackRouter_.onStateChanged.addListener(
        (status: Status, pairing: Pairing, details: Details) =>
            this.render_(status, pairing, details));

    const $ = this.$.bind(this);
    const value = (id: string) =>
        this.$<HTMLInputElement|HTMLTextAreaElement>(id).value.trim();

    $('mapProfiles').addEventListener('click', async () => {
      const entries: ProfileMappingEntry[] = [];
      for (const select of Array.from(
               $('mappingRows').querySelectorAll('select'))) {
        entries.push(
            {localId: select.dataset['localId'] || '', chainUuid: select.value});
      }
      const {ok, error} = await this.handler_.setProfileMapping(entries);
      $('mappingError').textContent = ok ? '' : error;
      this.refresh_();
    });

    $('saved').addEventListener('click', () => {
      $('words').replaceChildren();
      this.showingWords_ = false;
      this.show('wordsPanel', false);
      if (this.latest_) {
        this.render_(
            this.latest_.status, this.latest_.pairing, this.latest_.details);
      }
      this.refresh_();
    });

    $('start').addEventListener('click', async () => {
      $('offError').textContent = '';
      const {ok, error, words} = await this.handler_.startSync(value('server'));
      if (!ok) {
        $('offError').textContent = error;
        return;
      }
      this.showWords_(words, 'I saved it');
    });

    $('showJoin').addEventListener('click', () => {
      this.show('joinPanel', true);
      $('code').focus();
    });

    $('join').addEventListener('click', () => {
      $('offError').textContent = '';
      this.handler_.joinWithPairingCode(value('server'), value('code'));
    });

    $('showRecovery').addEventListener('click', () => {
      this.show('recoveryPanel', true);
      $('recovery').focus();
    });

    $('recover').addEventListener('click', async () => {
      $('offError').textContent = '';
      const {ok, error} = await this.handler_.joinWithRecoveryCode(
          value('server'), value('recovery'));
      if (!ok) {
        $('offError').textContent = error;
        return;
      }
      $<HTMLTextAreaElement>('recovery').value = '';
      this.refresh_();
    });

    $('addDevice').addEventListener('click', () => this.handler_.addDevice());
    $('allow').addEventListener(
        'click', () => this.handler_.answerPairing(true));
    $('deny').addEventListener(
        'click', () => this.handler_.answerPairing(false));
    $('cancelPairing')
        .addEventListener('click', () => this.handler_.cancelPairing());
    $('restart').addEventListener('click', () => this.handler_.restart());

    $('showWords').addEventListener('click', async () => {
      $('wordsError').textContent = '';
      const {ok, error, words} = await this.handler_.revealRecoveryCode();
      if (!ok) {
        // Empty when the user canceled Touch ID.
        $('wordsError').textContent = error;
        return;
      }
      this.showWords_(words, 'Hide');
    });

    $('leave').addEventListener('click', async () => {
      if (await this.confirmAction_(
              'Leave sync?',
              'This device keeps its data but stops syncing. Your other ' +
                  'devices go on syncing. Compass restarts to finish.',
              'Leave sync')) {
        this.handler_.leave();
      }
    });

    $('reset').addEventListener('click', async () => {
      $('onError').textContent = '';
      if (!await this.confirmAction_(
              'Reset sync?',
              'Compass makes a new sync chain with a new recovery code, and ' +
                  'this device\'s data goes up to it after a restart. Your ' +
                  'other devices stay on the old chain until you add them ' +
                  'again, and the old recovery code stops working here.',
              'Reset sync')) {
        return;
      }
      const {ok, error, words} = await this.handler_.resetSync();
      if (!ok) {
        $('onError').textContent = error;
        return;
      }
      this.showWords_(words, 'I saved it');
    });

    this.refresh_();
  }

  private renderDevices_(devices: Device[]) {
    this.$('devices').replaceChildren(...devices.map(device => {
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
    this.show('devices', devices.length > 0);
    this.show('noDevices', devices.length === 0);
  }

  private renderTypes_(types: DataTypeState[]) {
    const enabled = new Map(types.map(t => [t.type, t.enabled]));
    const list = this.$('types');
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
            'change',
            () => this.handler_.setDataTypeEnabled(type, toggle.checked));
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

  private render_(status: Status, pairing: Pairing, details: Details) {
    this.latest_ = {status, pairing, details};
    // For the end-to-end steps: the app menu's row for this state.
    document.documentElement.dataset['menuLabel'] = status.menuLabel;
    this.$('statusDot').dataset['state'] = stateName(status.state);
    this.$('status').textContent = status.message ||
        (status.state === State.kOff ? 'Sync is off.' : '');
    if (this.showingWords_) {
      return;
    }
    const state = status.state;
    const pairingActive = pairing.step !== PairingStep.kNone;
    const on = state === State.kOn || state === State.kStarting ||
        state === State.kError;
    this.show('off', state === State.kOff);
    this.show('restartPanel', state === State.kNeedsRestart);
    this.show('on', on && !pairingActive);
    this.$<HTMLButtonElement>('addDevice').disabled = state !== State.kOn;
    this.$('serverUrl').textContent = status.serverUrl;
    this.renderDevices_(details.devices);
    this.renderTypes_(details.types);

    this.show('pairing', pairingActive);
    this.$('pairingCode').textContent = pairing.code;
    this.renderQr_(pairing);
    this.renderExpiry_(pairing);
    this.$('pairingMessage').textContent = pairing.message;
    this.$('pairingMessage')
        .classList.toggle('error', pairing.step === PairingStep.kFailed);
    this.show('confirm', pairing.step === PairingStep.kConfirm);
    this.$('deviceName').textContent = pairing.deviceName;
    this.$('cancelPairing').textContent =
        pairing.step === PairingStep.kDone ||
            pairing.step === PairingStep.kFailed ?
        'Close' :
        'Cancel';
    if (pairing.step === PairingStep.kDone && state === State.kNeedsRestart) {
      this.show('restartPanel', true);
    }
    this.renderMapping_();
  }

  // "Expires in 4:12" under the code, once a second, while A shows it. At
  // zero the browser side ends the pairing ("The code expired…"), which
  // replaces the panel.
  private renderExpiry_(pairing: Pairing) {
    if (this.expiryTimer_ !== null) {
      clearInterval(this.expiryTimer_);
      this.expiryTimer_ = null;
    }
    const line = this.$('pairingExpiry');
    const expiresAt = pairing.expiresAtMs;
    const shown = pairing.step === PairingStep.kShowingCode && expiresAt > 0;
    this.show('pairingExpiry', shown);
    if (!shown) {
      line.textContent = '';
      return;
    }
    const tick = () => {
      const left = Math.max(0, Math.ceil((expiresAt - Date.now()) / 1000));
      const minutes = Math.floor(left / 60);
      const seconds = String(left % 60).padStart(2, '0');
      line.textContent =
          left > 0 ? `Expires in ${minutes}:${seconds}` : 'Expired';
      line.classList.toggle('soon', left <= 30);
    };
    tick();
    this.expiryTimer_ = setInterval(tick, 1000);
  }

  // The invite as a QR code while the code shows, for a phone's camera
  // (docs/design/ios.md). Dark on white whatever the theme: scanners need
  // the contrast, and the four-module quiet zone is part of the white.
  private renderQr_(pairing: Pairing) {
    const box = this.$('pairingQr');
    const size = pairing.qrSize;
    const shown = pairing.step === PairingStep.kShowingCode && size > 0 &&
        pairing.qr.length === size * size;
    this.show('pairingQrPanel', shown);
    if (!shown) {
      box.replaceChildren();
      delete box.dataset['invite'];
      this.qrShown_ = '';
      return;
    }
    if (this.qrShown_ === pairing.inviteUrl) {
      return;
    }
    this.qrShown_ = pairing.inviteUrl;
    const quiet = 4;
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    const extent = size + 2 * quiet;
    svg.setAttribute('viewBox', `0 0 ${extent} ${extent}`);
    svg.setAttribute('shape-rendering', 'crispEdges');
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', 'QR code to add a phone');
    const background = document.createElementNS(ns, 'rect');
    background.setAttribute('width', String(extent));
    background.setAttribute('height', String(extent));
    background.setAttribute('fill', '#fff');
    const path = document.createElementNS(ns, 'path');
    let d = '';
    for (let y = 0; y < size; ++y) {
      for (let x = 0; x < size; ++x) {
        if (pairing.qr[y * size + x]) {
          d += `M${x + quiet} ${y + quiet}h1v1h-1z`;
        }
      }
    }
    path.setAttribute('d', d);
    path.setAttribute('fill', '#000');
    svg.append(background, path);
    box.replaceChildren(svg);
    // For tools/sync_e2e/sync_status.js.
    box.dataset['invite'] = pairing.inviteUrl;
  }

  // Slice 5: this Mac's other profiles, each mapped to one of the chain's
  // or joining as a new one. The rows stay while the same profiles are
  // asked, so the user's choices aren't reset by a status change.
  private async renderMapping_() {
    const {mapping} = await this.handler_.getProfileMapping();
    this.show('mapping', mapping.needed);
    const key = mapping.needed ? JSON.stringify(mapping) : '';
    if (key === this.mappingShown_) {
      return;
    }
    this.mappingShown_ = key;
    this.$('mappingError').textContent = '';
    this.$('mappingRows').replaceChildren(...this.mappingRows_(mapping));
  }

  private mappingRows_(mapping: ProfileMapping): HTMLElement[] {
    return mapping.local.map(local => {
      const row = document.createElement('label');
      row.textContent = local.name + ' syncs with ';
      const select = document.createElement('select');
      select.dataset['localId'] = local.id;
      select.append(new Option('a new profile', ''));
      for (const profile of mapping.chain) {
        if (!profile.home) {
          select.append(new Option(profile.name, profile.uuid));
        }
      }
      const proposed =
          mapping.proposed.find(entry => entry.localId === local.id);
      select.value = proposed ? proposed.chainUuid : '';
      row.append(select);
      return row;
    });
  }

  private async refresh_() {
    const {status, pairing, details} = await this.handler_.getState();
    this.render_(status, pairing, details);
  }

  // Shows |words| alone on the page until the user dismisses them.
  private showWords_(words: string[], dismiss: string) {
    this.$('words').replaceChildren(...words.map(word => {
      const item = document.createElement('li');
      item.textContent = word;
      return item;
    }));
    this.$('saved').textContent = dismiss;
    this.showingWords_ = true;
    for (const id of ['off', 'on', 'pairing', 'restartPanel', 'mapping']) {
      this.show(id, false);
    }
    this.show('wordsPanel', true);
  }

  // A modal question; resolves to whether the user confirmed.
  private confirmAction_(title: string, text: string, ok: string):
      Promise<boolean> {
    const dialog = this.$<HTMLDialogElement>('confirmDialog');
    const okButton = this.$('confirmOk');
    const cancelButton = this.$('confirmCancel');
    this.$('confirmTitle').textContent = title;
    this.$('confirmText').textContent = text;
    okButton.textContent = ok;
    okButton.className = ok === 'Reset sync' ? 'danger' : '';
    return new Promise(resolve => {
      const done = (result: boolean) => {
        okButton.onclick = null;
        cancelButton.onclick = null;
        dialog.onclose = null;
        dialog.close();
        resolve(result);
      };
      okButton.onclick = () => done(true);
      cancelButton.onclick = () => done(false);
      dialog.onclose = () => resolve(false);
      dialog.showModal();
    });
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-sync-page': CompassSyncPageElement;
  }
}

customElements.define('compass-sync-page', CompassSyncPageElement);
