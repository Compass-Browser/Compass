// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import type {AddressDraft, CardDraft, SavedAddress, SavedCard, SavedPassword} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './passwords_pane.html.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';

export const PREF_OFFER_TO_SAVE = 'credentials_enable_service';
export const PREF_AUTO_SIGN_IN = 'credentials_enable_autosignin';
export const PREF_SAVE_CARDS = 'autofill.credit_card_enabled';
export const PREF_SAVE_ADDRESSES = 'autofill.profile_enabled';

// ISO 3166-1 alpha-2 codes, for the address editor's country menu (named by
// Intl.DisplayNames in the browser's language).
const COUNTRY_CODES = (
    'AD AE AF AG AI AL AM AO AQ AR AS AT AU AW AX AZ BA BB BD BE BF BG BH BI ' +
    'BJ BL BM BN BO BQ BR BS BT BV BW BY BZ CA CC CD CF CG CH CI CK CL CM CN ' +
    'CO CR CU CV CW CX CY CZ DE DJ DK DM DO DZ EC EE EG EH ER ES ET FI FJ FK ' +
    'FM FO FR GA GB GD GE GF GG GH GI GL GM GN GP GQ GR GS GT GU GW GY HK HM ' +
    'HN HR HT HU ID IE IL IM IN IO IQ IR IS IT JE JM JO JP KE KG KH KI KM KN ' +
    'KP KR KW KY KZ LA LB LC LI LK LR LS LT LU LV LY MA MC MD ME MF MG MH MK ' +
    'ML MM MN MO MP MQ MR MS MT MU MV MW MX MY MZ NA NC NE NF NG NI NL NO NP ' +
    'NR NU NZ OM PA PE PF PG PH PK PL PM PN PR PS PT PW PY QA RE RO RS RU RW ' +
    'SA SB SC SD SE SG SH SI SJ SK SL SM SN SO SR SS ST SV SX SY SZ TC TD TF ' +
    'TG TH TJ TK TL TM TN TO TR TT TV TW TZ UA UG UM US UY UZ VA VC VE VG VI ' +
    'VN VU WF WS YE YT ZA ZM ZW')
                           .split(' ');

export interface Country {
  code: string;
  name: string;
}

function listCountries(): Country[] {
  let names: Intl.DisplayNames|null = null;
  try {
    names = new Intl.DisplayNames([navigator.language], {type: 'region'});
  } catch {
    // Codes, then.
  }
  return COUNTRY_CODES.map(code => ({code, name: names?.of(code) || code}))
      .sort((a, b) => a.name.localeCompare(b.name));
}

// The browser's region ("en-US" → "US"), for a new address.
function defaultCountry(): string {
  const region = navigator.language.split('-')[1];
  return region && COUNTRY_CODES.includes(region.toUpperCase()) ?
      region.toUpperCase() :
      'US';
}

// The pane's views: its main one, and each list on a page of its own
// (?view=passwords, cards, addresses).
export type PasswordsView = 'main'|'passwords'|'cards'|'addresses';
const VIEWS: PasswordsView[] = ['main', 'passwords', 'cards', 'addresses'];

// The colors of the sites' letter tiles (window.css, .tile), picked by site.
const SITE_COLORS = ['blue', 'green', 'orange', 'purple', 'pink', 'teal',
                     'indigo'];

// How long a revealed password stays shown.
const REVEAL_MILLISECONDS = 30000;

// Passwords (docs/design/settings-window.md, phase 4): saved passwords and
// passkeys (shown after the system's authentication, copied without the page
// seeing them), payment cards and addresses, with editors for cards and
// addresses. Adding and editing passwords, the checkup and import/export are
// the password manager's (chrome://password-manager).
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
      cardDraft_: {type: Object},
      addressDraft_: {type: Object},
      draftError_: {type: String},
      view_: {type: String},
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
  // The card or address being added or edited, if any.
  protected accessor cardDraft_: CardDraft|null = null;
  protected accessor addressDraft_: AddressDraft|null = null;
  protected accessor draftError_: string = '';
  protected accessor view_: PasswordsView = 'main';

  protected readonly countries = listCountries();
  protected readonly months = Array.from({length: 12}, (_, i) => i + 1);
  protected readonly years = Array.from(
      {length: 16}, (_, i) => new Date().getFullYear() + i);

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  private listenerIds_: number[] = [];
  // What the path asked to scroll to (?focus=saveCards, saveAddresses).
  private focus_ = '';

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    const params = new URLSearchParams(window.location.search);
    this.focus_ = params.get('focus') || '';
    const view = params.get('view') as PasswordsView|null;
    if (view && VIEWS.includes(view)) {
      this.setView_(view);
    }
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

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties);
    if (this.focus_) {
      this.querySelector(`[data-row="${CSS.escape(this.focus_)}"]`)
          ?.scrollIntoView({block: 'start'});
      this.focus_ = '';
    }
  }

  // Template helpers.

  protected isView_(view: PasswordsView): boolean {
    return this.view_ === view;
  }

  protected getSiteInitial_(site: string): string {
    return (site.replace(/^www\./, '')[0] || '?').toUpperCase();
  }

  protected getSiteColor_(site: string): string {
    let hash = 0;
    for (const char of site) {
      hash = (hash * 31 + char.charCodeAt(0)) >>> 0;
    }
    return SITE_COLORS[hash % SITE_COLORS.length]!;
  }

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

  // The list's caption: "57 passwords", "1 password".
  protected getPasswordsTitle_(): string {
    const count = this.passwords_.length;
    return count === 1 ? '1 password' : `${count} passwords`;
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

  protected isNewCard_(): boolean {
    return !this.cardDraft_?.guid;
  }

  protected getCardDraftTitle_(): string {
    return this.isNewCard_() ? 'Add a card' :
                               `Edit card •••• ${this.cardDraft_!.lastFour}`;
  }

  protected getAddressDraftTitle_(): string {
    return this.addressDraft_?.guid ? 'Edit address' : 'Add an address';
  }

  protected getMonthLabel_(month: number): string {
    return String(month).padStart(2, '0');
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

  // The card editor.

  protected onAddCardClick_() {
    this.addressDraft_ = null;
    this.draftError_ = '';
    const now = new Date();
    this.cardDraft_ = {
      guid: '',
      nameOnCard: '',
      number: '',
      expirationMonth: now.getMonth() + 1,
      expirationYear: now.getFullYear() + 1,
      nickname: '',
      lastFour: '',
    };
  }

  protected async onEditCardClick_(e: Event) {
    const guid = (e.currentTarget as HTMLElement).dataset['guid']!;
    const {card} = await this.handler_.getCard(guid);
    if (card) {
      this.addressDraft_ = null;
      this.draftError_ = '';
      this.cardDraft_ = card;
    }
  }

  // A text field of the card (data-field: nameOnCard, number, nickname).
  protected onCardInput_(e: Event) {
    const input = e.target as HTMLInputElement;
    this.cardDraft_ = {...this.cardDraft_!, [input.dataset['field']!]: input.value};
    this.draftError_ = '';
  }

  // The expiry's month or year (data-field).
  protected onCardExpiryChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    this.cardDraft_ = {
      ...this.cardDraft_!,
      [select.dataset['field']!]: Number(select.value),
    };
    this.draftError_ = '';
  }

  protected async onSaveCardClick_() {
    const {error} = await this.handler_.saveCard(this.cardDraft_!);
    if (error) {
      this.draftError_ = error;
      return;
    }
    this.cardDraft_ = null;
  }

  // The address editor.

  protected onAddAddressClick_() {
    this.cardDraft_ = null;
    this.draftError_ = '';
    this.addressDraft_ = {
      guid: '',
      fullName: '',
      organization: '',
      street: '',
      city: '',
      state: '',
      postalCode: '',
      countryCode: defaultCountry(),
      phone: '',
      email: '',
    };
  }

  protected async onEditAddressClick_(e: Event) {
    const guid = (e.currentTarget as HTMLElement).dataset['guid']!;
    const {address} = await this.handler_.getAddress(guid);
    if (address) {
      this.cardDraft_ = null;
      this.draftError_ = '';
      this.addressDraft_ = {
        ...address,
        countryCode: address.countryCode || defaultCountry(),
      };
    }
  }

  // A field of the address (data-field).
  protected onAddressInput_(e: Event) {
    const input = e.target as HTMLInputElement | HTMLTextAreaElement |
        HTMLSelectElement;
    this.addressDraft_ = {
      ...this.addressDraft_!,
      [input.dataset['field']!]: input.value,
    };
    this.draftError_ = '';
  }

  protected onCountryChange_(e: Event) {
    this.onAddressInput_(e);
  }

  protected async onSaveAddressClick_() {
    const {error} = await this.handler_.saveAddress(this.addressDraft_!);
    if (error) {
      this.draftError_ = error;
      return;
    }
    this.addressDraft_ = null;
  }

  protected onCancelDraftClick_() {
    this.cardDraft_ = null;
    this.addressDraft_ = null;
    this.draftError_ = '';
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

  // Views.

  protected onViewClick_(e: Event) {
    this.setView_(
        (e.currentTarget as HTMLElement).dataset['view'] as PasswordsView);
  }

  protected onBackClick_() {
    this.setView_('main');
  }

  private setView_(view: PasswordsView) {
    this.view_ = view;
    // An editor doesn't stay open on another view.
    this.cardDraft_ = null;
    this.addressDraft_ = null;
    this.draftError_ = '';
    window.history.replaceState(
        null, '', view === 'main' ? '/passwords' : `/passwords?view=${view}`);
    // The window hides the pane's header while a view is shown (app.ts).
    this.toggleAttribute('nested', view !== 'main');
    this.dispatchEvent(new CustomEvent('settings-view', {bubbles: true}));
    this.closest('#scroller')?.scrollTo(0, 0);
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
