// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {SavedAddress, SavedCard, SavedPassword} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './passwords_pane.html.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';

export const PREF_OFFER_TO_SAVE = 'credentials_enable_service';
export const PREF_AUTO_SIGN_IN = 'credentials_enable_autosignin';
export const PREF_SAVE_CARDS = 'autofill.credit_card_enabled';
export const PREF_SAVE_ADDRESSES = 'autofill.profile_enabled';

// How long a revealed password stays shown.
const REVEAL_MILLISECONDS = 30000;

// Passwords (docs/design/settings-window.md, phase 4): saved passwords and
// passkeys (shown after the system's authentication, copied without the page
// seeing them), payment cards and addresses. Adding and editing them, the
// checkup and import/export are the password manager's
// (chrome://password-manager) and upstream's payments and addresses
// sections.
export class CompassSettingsPasswordsPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-passwords-pane';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      passwords_: {type: Array},
      filter_: {type: String},
      revealed_: {type: Object},
      copied_: {type: Number},
      cards_: {type: Array},
      addresses_: {type: Array},
      prefsVersion_: {type: Number},
    };
  }

  protected accessor passwords_: SavedPassword[] = [];
  protected accessor filter_: string = '';
  // Passwords shown, by id.
  protected accessor revealed_: Record<number, string> = {};
  // The password just copied, for a moment.
  protected accessor copied_: number = -1;
  protected accessor cards_: SavedCard[] = [];
  protected accessor addresses_: SavedAddress[] = [];
  protected accessor prefsVersion_: number = 0;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  private listenerIds_: number[] = [];

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    prefs.addListener(this.prefListener_);
    prefs.load([
           PREF_OFFER_TO_SAVE,
           PREF_AUTO_SIGN_IN,
           PREF_SAVE_CARDS,
           PREF_SAVE_ADDRESSES,
         ])
        .then(() => this.prefsVersion_++);
    const router = SettingsWindowBrowserProxy.getInstance().callbackRouter;
    this.listenerIds_ = [
      router.passwordsChanged.addListener(() => this.loadPasswords_()),
      router.autofillChanged.addListener(() => this.loadAutofill_()),
    ];
    this.loadPasswords_();
    this.loadAutofill_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    prefs.removeListener(this.prefListener_);
    const router = SettingsWindowBrowserProxy.getInstance().callbackRouter;
    for (const id of this.listenerIds_) {
      router.removeListener(id);
    }
    this.listenerIds_ = [];
    // Nothing stays shown once the pane goes.
    this.revealed_ = {};
  }

  // Template helpers.

  protected getBool_(name: string): boolean {
    return prefs.get(name)?.value === true;
  }

  protected isEnforced_(name: string): boolean {
    return !!prefs.get(name)?.enforced;
  }

  // The passwords whose site or username has the filter's text.
  protected getShownPasswords_(): SavedPassword[] {
    const filter = this.filter_.trim().toLowerCase();
    if (!filter) {
      return this.passwords_;
    }
    return this.passwords_.filter(
        p => p.site.toLowerCase().includes(filter) ||
            p.username.toLowerCase().includes(filter));
  }

  protected getPasswordsDetail_(): string {
    return String(this.passwords_.length);
  }

  protected isRevealed_(password: SavedPassword): boolean {
    return password.id in this.revealed_;
  }

  protected getSecret_(password: SavedPassword): string {
    if (password.isPasskey) {
      return 'Passkey';
    }
    return this.revealed_[password.id] ?? '••••••••';
  }

  protected getRevealLabel_(password: SavedPassword): string {
    return this.isRevealed_(password) ? 'Hide' : 'Show';
  }

  protected getCopyLabel_(password: SavedPassword): string {
    return this.copied_ === password.id ? 'Copied' : 'Copy';
  }

  protected getCardText_(card: SavedCard): string {
    return `${card.name} •••• ${card.lastFour}`;
  }

  // Events.

  protected onFilterInput_(e: Event) {
    this.filter_ = (e.target as HTMLInputElement).value;
  }

  protected onSwitchChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    prefs.set(input.dataset['pref']!, input.checked);
  }

  protected async onRevealClick_(e: Event) {
    const id = Number((e.currentTarget as HTMLElement).dataset['id']);
    if (id in this.revealed_) {
      this.hide_(id);
      return;
    }
    // The system asks the user to authenticate first.
    const {password} = await this.handler_.revealPassword(id);
    if (password === null || password === undefined) {
      return;
    }
    this.revealed_ = {...this.revealed_, [id]: password};
    setTimeout(() => this.hide_(id), REVEAL_MILLISECONDS);
  }

  protected async onCopyClick_(e: Event) {
    const id = Number((e.currentTarget as HTMLElement).dataset['id']);
    const {copied} = await this.handler_.copyPassword(id);
    if (copied) {
      this.copied_ = id;
      setTimeout(() => {
        if (this.copied_ === id) {
          this.copied_ = -1;
        }
      }, 2000);
    }
  }

  protected onRemovePasswordClick_(e: Event) {
    const id = Number((e.currentTarget as HTMLElement).dataset['id']);
    this.hide_(id);
    this.handler_.removePassword(id);
  }

  protected onRemoveCardClick_(e: Event) {
    this.handler_.removeCard((e.currentTarget as HTMLElement).dataset['guid']!);
  }

  protected onRemoveAddressClick_(e: Event) {
    this.handler_.removeAddress(
        (e.currentTarget as HTMLElement).dataset['guid']!);
  }

  protected onPasswordManagerClick_() {
    this.handler_.openUrl('chrome://password-manager/passwords');
  }

  // Upstream's sections for adding and editing (data-sub-page).
  protected onChromeSettingsClick_(e: Event) {
    this.handler_.openChromeSettings(
        (e.currentTarget as HTMLElement).dataset['subPage']!);
  }

  private hide_(id: number) {
    if (!(id in this.revealed_)) {
      return;
    }
    const revealed = {...this.revealed_};
    delete revealed[id];
    this.revealed_ = revealed;
  }

  private async loadPasswords_() {
    const {passwords} = await this.handler_.getPasswords();
    this.passwords_ = [...passwords].sort(
        (a, b) => a.site.localeCompare(b.site) ||
            a.username.localeCompare(b.username));
  }

  private async loadAutofill_() {
    const [{cards}, {addresses}] = await Promise.all(
        [this.handler_.getCards(), this.handler_.getAddresses()]);
    this.cards_ = cards;
    this.addresses_ = addresses;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-passwords-pane': CompassSettingsPasswordsPaneElement;
  }
}

customElements.define(
    CompassSettingsPasswordsPaneElement.is,
    CompassSettingsPasswordsPaneElement);
