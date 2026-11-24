// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {DataTypeState, Details, Device, Login, Pairing, ProfileMapping, ProfileMappingEntry, ServerPeople, ServerUpdate, Status} from '../compass_sync.mojom-webui.js';
import {DataType, PageCallbackRouter, PageHandlerFactory, PageHandlerRemote, PairingStep, State} from '../compass_sync.mojom-webui.js';

import {CompassSettingsPage} from './page_base.js';
import {getTemplate} from './sync_page.html.js';

// Sync: compass://settings/sync, and alone on chrome://compass-sync
// (docs/design/sync.md, "UI"). Off: start a chain on a
// server, or join one by pairing code or recovery code. On: the status, the
// devices, adding one, what syncs, the recovery code (behind Touch ID), the
// server, leaving and resetting, removing another device. Removed on
// another device: only leaving. Joining on a Mac with other profiles: which
// of the chain's each one syncs with (slice 5). A server that asks for
// company sign-in: the sign-in, its code, and who the chain is signed in as.

// The switches, in the page's order: name and what they cover. Bookmarks
// and the reading list have none: Compass has neither (ARCHITECTURE.md
// D20), so those types keep their setting out of sight.
const TYPE_LABELS = new Map<DataType, [string, string]>([
  [DataType.kSpaces, ['Spaces', 'Spaces, pinned tabs, folders, favorites']],
  [DataType.kPasswords, ['Passwords', 'Saved passwords and passkeys']],
  [DataType.kHistory, ['History', 'Pages you visited']],
  [DataType.kAutofill, ['Autofill', 'Addresses and form entries']],
  [DataType.kExtensions, ['Extensions', 'Extensions and their settings']],
  [DataType.kSettings, ['Settings', 'Settings and search engines']],
  [DataType.kOpenTabs, ['Open tabs', 'Your tabs, for the other devices']],
]);

// The buttons that start setting sync up, disabled while a setup runs.
const SETUP_BUTTONS = [
  'start',
  'join',
  'recover',
  'startCloudflare',
  'signInCloudflare',
  'joinInvite',
];

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
    case State.kLoginRequired:
      return 'login';
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
  // The server whose people the page has loaded (only when sync is on), so
  // a status change doesn't ask again.
  private peopleLoadedFor_ = '';

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

    // docs/design/sync.md, "Inviting friends".
    $('showInvite').addEventListener('click', () => {
      this.show('invitePanel', true);
      $('inviteJoinLink').focus();
    });

    $('joinInvite').addEventListener('click', async () => {
      $('offError').textContent = '';
      const {ok, error, words} =
          await this.handler_.joinWithInvite(value('inviteJoinLink'));
      if (!ok) {
        $('offError').textContent = error;
        return;
      }
      $<HTMLInputElement>('inviteJoinLink').value = '';
      this.showWords_(words, 'I saved it');
    });

    $('invite').addEventListener('click', async () => {
      $('peopleError').textContent = '';
      const {ok, error, link} =
          await this.handler_.inviteFriend(value('inviteName'));
      if (!ok) {
        $('peopleError').textContent = error;
        return;
      }
      $<HTMLInputElement>('inviteName').value = '';
      $<HTMLInputElement>('inviteLink').value = link;
      this.show('inviteResult', true);
      $<HTMLInputElement>('inviteLink').select();
      this.loadPeople_();
    });

    $('copyInvite').addEventListener('click', async () => {
      const link = this.$<HTMLInputElement>('inviteLink');
      try {
        await navigator.clipboard.writeText(link.value);
        $('copyInvite').textContent = 'Copied';
        setTimeout(() => $('copyInvite').textContent = 'Copy link', 2000);
      } catch {
        link.select();
      }
    });

    // docs/design/sync.md, "Updating a server".
    const update = async (token: string) => {
      $('updateError').textContent = '';
      const {ok, error} = await this.handler_.updateServer(token);
      if (!ok) {
        $('updateError').textContent = error;
        return;
      }
      $<HTMLInputElement>('updateToken').value = '';
      this.loadServerUpdate_();
    };
    $('updateServer').addEventListener('click', () => update(''));
    $('updateServerToken')
        .addEventListener('click', () => update(value('updateToken')));
    $('showUpdateToken').addEventListener('click', () => {
      this.show('updateTokenPanel', true);
      $('updateToken').focus();
    });

    // docs/design/sync.md, "A free server on Cloudflare".
    $('showCloudflare').addEventListener('click', () => {
      this.show('cloudflarePanel', true);
      $('cloudflareToken').focus();
    });

    $('showCloudflareToken').addEventListener('click', () => {
      this.show('cloudflareTokenPanel', true);
      $('cloudflareToken').focus();
    });

    $('signInCloudflare').addEventListener('click', async () => {
      $('offError').textContent = '';
      const {ok, error, words} =
          await this.handler_.startOnCloudflareSignIn();
      if (!ok) {
        $('offError').textContent = error;
        return;
      }
      this.showWords_(words, 'I saved it');
    });

    $('startCloudflare').addEventListener('click', async () => {
      $('offError').textContent = '';
      const {ok, error, words} =
          await this.handler_.startOnCloudflare(value('cloudflareToken'));
      if (!ok) {
        $('offError').textContent = error;
        return;
      }
      $<HTMLInputElement>('cloudflareToken').value = '';
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

    $('startLogin').addEventListener('click', () => this.handler_.startLogin());
    // Again while it runs: the page opens again.
    $('reopenLogin')
        .addEventListener('click', () => this.handler_.startLogin());
    $('cancelLogin')
        .addEventListener('click', () => this.handler_.cancelLogin());

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

    $('removedLeave').addEventListener('click', () => this.handler_.leave());

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
      if (!device.thisDevice) {
        // docs/design/sync.md, "Removing a device".
        const remove = document.createElement('button');
        remove.className = 'secondary small';
        remove.textContent = 'Remove';
        remove.setAttribute(
            'aria-label', `Remove ${device.name || 'Unnamed device'}`);
        remove.addEventListener('click', () => this.removeDevice_(device));
        item.append(remove);
      }
      item.dataset['thisDevice'] = String(device.thisDevice);
      item.dataset['guid'] = device.guid;
      return item;
    }));
    this.show('devices', devices.length > 0);
    this.show('noDevices', devices.length === 0);
  }

  private async loadPeople_() {
    const {people} = await this.handler_.getServerPeople();
    this.renderPeople_(people);
    if (people.available) {
      this.loadServerUpdate_();
    }
  }

  // The owner's offer to update a server Compass set up on Cloudflare.
  private async loadServerUpdate_() {
    const {update} = await this.handler_.getServerUpdate();
    this.renderServerUpdate_(update);
  }

  private renderServerUpdate_(update: ServerUpdate) {
    this.show('serverUpdate', update.available);
    if (!update.available) {
      return;
    }
    // What each version brings, for the versions the server lacks.
    const NEW_IN: Record<number, string> = {
      2: 'sharing spaces, folders and easels with others',
      3: 'notifications on phones when a tab is sent to them, through ' +
          'Compass\'s push relay at push.compassbrowser.org',
    };
    const added = [];
    for (let v = update.current + 1; v <= update.latest; ++v) {
      if (NEW_IN[v]) {
        added.push(NEW_IN[v]);
      }
    }
    this.$('serverUpdateNote').textContent =
        (added.length ? `It adds ${added.join(', ')}. ` : '') +
        'Your data and the people on it stay as they are.';
  }

  // The friends and open invites of the server this chain owns; nothing on
  // a server without an access list, or one the chain was invited to.
  private renderPeople_(people: ServerPeople) {
    this.show('people', people.available);
    if (!people.available) {
      return;
    }
    const date = (ms: number) =>
        new Date(ms).toLocaleDateString(undefined, {dateStyle: 'medium'});
    const row = (title: string, details: string, action: string,
                 target: string, confirm: [string, string] | null) => {
      const item = document.createElement('li');
      const text = document.createElement('div');
      text.className = 'grow';
      const name = document.createElement('div');
      name.className = 'name';
      name.textContent = title;
      const info = document.createElement('div');
      info.className = 'details';
      info.textContent = details;
      text.append(name, info);
      const button = document.createElement('button');
      button.className = 'secondary small';
      button.textContent = action;
      button.setAttribute('aria-label', `${action} ${title}`);
      button.addEventListener('click', async () => {
        if (confirm &&
            !await this.confirmAction_(confirm[0], confirm[1], 'Remove')) {
          return;
        }
        this.$('peopleError').textContent = '';
        const {ok, error} = await this.handler_.removeFromServer(target);
        if (!ok) {
          this.$('peopleError').textContent = error;
        }
        this.loadPeople_();
      });
      item.append(text, button);
      return item;
    };
    const rows = [
      ...people.members.map(m => {
        // Their other profiles, and the chains they reset to.
        const profiles = m.linked > 0 ?
            ` · ${m.linked} linked account${m.linked === 1 ? '' : 's'}` :
            '';
        const title = m.name || 'Friend';
        return row(
            title, `Joined ${date(m.addedMs)}${profiles}`, 'Remove', m.chain,
            [
              `Remove ${title}?`,
              'They stop syncing on your server within a minute. Their ' +
                  'data stays on their devices. You can invite them again.',
            ]);
      }),
      ...people.invites.map(
          i => row(
              i.name ? `Invite for ${i.name}` : 'Invite',
              `Not used yet · expires ${date(i.expiresMs)}`, 'Revoke', i.code,
              null)),
    ];
    this.$('peopleList').replaceChildren(...rows);
    this.show('peopleList', rows.length > 0);
    this.show('noPeople', rows.length === 0);
  }

  // Asks, then removes |device| from the chain on the server. The list
  // drops it once the browser side has; errors show under the list.
  private async removeDevice_(device: Device) {
    const name = device.name || 'Unnamed device';
    this.$('devicesError').textContent = '';
    if (!await this.confirmAction_(
            `Remove ${name}?`,
            'It stops syncing and disappears from your devices. What\'s on ' +
                'it stays there, and it can join again with a pairing code. ' +
                'Someone who has your recovery code can still join: to lock ' +
                'everyone out, use Reset sync.',
            'Remove')) {
      return;
    }
    const {ok, error} = await this.handler_.removeDevice(device.guid);
    this.$('devicesError').textContent = ok ? '' : error;
    this.refresh_();
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
    const removed = status.deviceRemoved;
    const on = !removed &&
        (state === State.kOn || state === State.kStarting ||
         state === State.kError || state === State.kLoginRequired);
    this.renderLogin_(state, status.login);
    // A server being set up on Cloudflare: its progress, and nothing else
    // to start meanwhile.
    const settingUp = !!status.serverSetup;
    this.$('cloudflareProgress').textContent = status.serverSetup;
    // An update of the server runs while sync is on.
    this.$('updateProgress').textContent = status.serverSetup;
    for (const id of ['updateServer', 'updateServerToken']) {
      this.$<HTMLButtonElement>(id).disabled = settingUp;
    }
    if (settingUp) {
      this.show('cloudflarePanel', true);
      for (const id of
               SETUP_BUTTONS) {
        this.$<HTMLButtonElement>(id).disabled = true;
      }
    }
    this.show('off', state === State.kOff);
    this.show(
        'restartPanel',
        state === State.kNeedsRestart ||
            (state === State.kStarting && status.offerRestart));
    this.show('on', on && !pairingActive);
    this.show('removedPanel', removed);
    this.$<HTMLButtonElement>('addDevice').disabled = state !== State.kOn;
    this.$('serverUrl').textContent = status.serverUrl;
    this.renderDevices_(details.devices);
    // Who may use the server: asked once per server while sync is on.
    if (state === State.kOn && !removed &&
        this.peopleLoadedFor_ !== status.serverUrl) {
      this.peopleLoadedFor_ = status.serverUrl;
      this.loadPeople_();
    } else if (state !== State.kOn) {
      this.peopleLoadedFor_ = '';
      this.show('people', false);
    }
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

  // Company sign-in: the button while the chain must sign in, the code while
  // a sign-in runs (on #off too: starting or joining a chain signs in
  // first), and who the chain is signed in as.
  private renderLogin_(state: State, login: Login) {
    const provider = login.providerName;
    this.show(
        'loginRequired', state === State.kLoginRequired && !login.inProgress);
    this.$('startLogin').textContent =
        provider ? `Sign in to ${provider}` : 'Sign in';
    this.show('loginPanel', login.inProgress);
    this.$('loginTitle').textContent =
        provider ? `Sign in with ${provider}` : 'Sign in';
    this.$('loginCode').textContent = login.userCode;
    this.$<HTMLButtonElement>('reopenLogin').disabled = !login.userCode;
    for (const id of
             SETUP_BUTTONS) {
      this.$<HTMLButtonElement>(id).disabled = login.inProgress;
    }
    const user = login.userName && login.userEmail ?
        `${login.userName} (${login.userEmail})` :
        login.userName || login.userEmail;
    this.show('signedInAs', !!user);
    this.$('signedInAs').textContent = user ? `Signed in as ${user}.` : '';
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
    for (const id of
             ['off', 'on', 'pairing', 'restartPanel', 'mapping',
              'removedPanel', 'loginRequired', 'loginPanel']) {
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
    okButton.className =
        ok === 'Reset sync' || ok === 'Remove' ? 'danger' : '';
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
