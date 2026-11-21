// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import type {PermissionDefault, SitePermission, SiteSummary} from '../sidebar.mojom-webui.js';
import {PermissionValue} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';
import {getHtml} from './privacy_pane.html.js';

export const PREF_COOKIE_CONTROLS = 'profile.cookie_controls_mode';
export const PREF_DO_NOT_TRACK = 'enable_do_not_track';
export const PREF_HTTPS_ONLY = 'https_only_mode_enabled';
export const PREF_SECURE_DNS = 'dns_over_https.mode';
const PREF_SAFE_BROWSING = 'safebrowsing.enabled';
const PREF_SAFE_BROWSING_ENHANCED = 'safebrowsing.enhanced';
const PREF_CLEAR_PERIOD = 'browser.clear_data.time_period';

// Privacy › Clear browsing data: each choice and the pref it's kept in.
export interface ClearChoice {
  key: 'history'|'downloads'|'cookies'|'cache'|'passwords'|'formData'|
      'siteSettings';
  pref: string;
  label: string;
  detail: string;
}

export const CLEAR_CHOICES: ClearChoice[] = [
  {
    key: 'history',
    pref: 'browser.clear_data.browsing_history',
    label: 'Browsing history',
    detail: 'Pages you visited, on this device and those you sync with.',
  },
  {
    key: 'downloads',
    pref: 'browser.clear_data.download_history',
    label: 'Download history',
    detail: 'The list of downloads; the files stay.',
  },
  {
    key: 'cookies',
    pref: 'browser.clear_data.cookies',
    label: 'Cookies and other site data',
    detail: 'Signs you out of most sites.',
  },
  {
    key: 'cache',
    pref: 'browser.clear_data.cache',
    label: 'Cached images and files',
    detail: 'Some sites may load more slowly next time.',
  },
  {
    key: 'passwords',
    pref: 'browser.clear_data.passwords',
    label: 'Passwords and passkeys',
    detail: 'Saved in this profile.',
  },
  {
    key: 'formData',
    pref: 'browser.clear_data.form_data',
    label: 'Autofill form data',
    detail: 'Addresses and other things you typed in forms.',
  },
  {
    key: 'siteSettings',
    pref: 'browser.clear_data.site_settings',
    label: 'Site settings',
    detail: 'Permissions you gave or refused sites.',
  },
];

export interface Choice {
  value: number;
  label: string;
}

// browsing_data::TimePeriod.
export const TIME_PERIODS: Choice[] = [
  {value: 6, label: 'Last 15 minutes'},
  {value: 0, label: 'Last hour'},
  {value: 1, label: 'Last 24 hours'},
  {value: 2, label: 'Last 7 days'},
  {value: 3, label: 'Last 4 weeks'},
  {value: 4, label: 'All time'},
];

// content_settings::CookieControlsMode.
export const COOKIE_MODES: Choice[] = [
  {value: 0, label: 'Allow'},
  {value: 2, label: 'Block in Incognito'},
  {value: 1, label: 'Block'},
];

// safe_browsing::SafeBrowsingState.
export const SAFE_BROWSING_LEVELS: Choice[] = [
  {value: 2, label: 'Enhanced'},
  {value: 1, label: 'Standard'},
  {value: 0, label: 'Off'},
];

// The permissions by upstream's group names, as the window names them.
const PERMISSION_LABELS: Record<string, string> = {
  'location': 'Location',
  'media-stream-camera': 'Camera',
  'media-stream-mic': 'Microphone',
  'notifications': 'Notifications',
  'popups': 'Pop-ups and redirects',
  'javascript': 'JavaScript',
  'javascript-optimizer': 'JavaScript optimization',
  'images': 'Images',
  'sound': 'Sound',
  'multiple-automatic-downloads': 'Automatic downloads',
  'clipboard': 'Clipboard',
  'sensors': 'Motion sensors',
  'midi-sysex': 'MIDI devices',
  'usb-devices': 'USB devices',
  'serial-ports': 'Serial ports',
  'hid-devices': 'HID devices',
  'bluetooth-scanning': 'Bluetooth scanning',
  'file-system-write': 'File editing',
  'window-management': 'Window management',
  'local-fonts': 'Fonts',
  'protected-content': 'Protected content IDs',
  'mixed-script': 'Insecure content',
  'background-sync': 'Background sync',
  'payment-handler': 'Payment handlers',
  'idle-detection': 'Your device use',
  'storage-access': 'Embedded content',
  'federated-identity-api': 'Third-party sign-in',
  'auto-picture-in-picture': 'Automatic picture-in-picture',
  'vr': 'Virtual reality',
  'ar': 'Augmented reality',
  'smart-card-readers': 'Smart card readers',
};

// The permissions' order in the lists: the common ones first, as in the
// labels above; any other after them.
const PERMISSION_ORDER = Object.keys(PERMISSION_LABELS);

function byPermissionOrder(a: {type: string}, b: {type: string}): number {
  const rank = (type: string) => {
    const index = PERMISSION_ORDER.indexOf(type);
    return index === -1 ? PERMISSION_ORDER.length : index;
  };
  return rank(a.type) - rank(b.type);
}

export type View = 'main'|'clear'|'permissions'|'sites'|'site';

// Privacy (docs/design/settings-window.md, phase 3): clearing browsing data,
// cookies, security, and site settings (the permissions' defaults, the sites
// with permissions of their own, one site's), each a view of the pane with a
// way back. Safety check still opens upstream's section.
export class CompassSettingsPrivacyPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-privacy-pane';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      view_: {type: String},
      prefsVersion_: {type: Number},
      defaults_: {type: Array},
      sites_: {type: Array},
      site_: {type: String},
      siteName_: {type: String},
      sitePermissions_: {type: Array},
      clearChoices_: {type: Object},
      clearPeriod_: {type: Number},
      clearing_: {type: Boolean},
      cleared_: {type: Boolean},
      siteDataCleared_: {type: Boolean},
    };
  }

  protected accessor view_: View = 'main';
  protected accessor prefsVersion_: number = 0;
  protected accessor defaults_: PermissionDefault[] = [];
  protected accessor sites_: SiteSummary[] = [];
  protected accessor site_: string = '';
  protected accessor siteName_: string = '';
  protected accessor sitePermissions_: SitePermission[] = [];
  protected accessor clearChoices_: Record<string, boolean> = {};
  protected accessor clearPeriod_: number = 4;
  protected accessor clearing_: boolean = false;
  protected accessor cleared_: boolean = false;
  protected accessor siteDataCleared_: boolean = false;

  protected readonly clearChoices = CLEAR_CHOICES;
  protected readonly timePeriods = TIME_PERIODS;
  protected readonly cookieModes = COOKIE_MODES;
  protected readonly safeBrowsingLevels = SAFE_BROWSING_LEVELS;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  private contentListenerId_: number|null = null;
  // What the path asked to scroll to ("security", "cookies").
  private focus_ = '';

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    // The path's query picks the view: ?view=clear|permissions|sites,
    // ?site=<origin>, ?focus=security|cookies.
    const params = new URLSearchParams(window.location.search);
    const site = params.get('site');
    const view = params.get('view');
    this.focus_ = params.get('focus') || '';
    if (site) {
      this.openSite_(site);
    } else if (view === 'clear' || view === 'permissions' ||
               view === 'sites') {
      this.setView_(view);
    } else {
      this.loadView_();
    }
    prefs.addListener(this.prefListener_);
    prefs.load([
      PREF_COOKIE_CONTROLS,
      PREF_DO_NOT_TRACK,
      PREF_HTTPS_ONLY,
      PREF_SECURE_DNS,
      PREF_SAFE_BROWSING,
      PREF_SAFE_BROWSING_ENHANCED,
      PREF_CLEAR_PERIOD,
      ...CLEAR_CHOICES.map(c => c.pref),
    ]).then(() => {
      this.resetClearChoices_();
      this.prefsVersion_++;
    });
    const router = SettingsWindowBrowserProxy.getInstance().callbackRouter;
    this.contentListenerId_ = router.contentSettingsChanged.addListener(
        () => this.onContentSettingsChanged_());
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    prefs.removeListener(this.prefListener_);
    if (this.contentListenerId_ !== null) {
      SettingsWindowBrowserProxy.getInstance().callbackRouter.removeListener(
          this.contentListenerId_);
      this.contentListenerId_ = null;
    }
  }

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties);
    if (this.focus_ && this.view_ === 'main') {
      this.querySelector(`[data-row="${CSS.escape(this.focus_)}"]`)
          ?.scrollIntoView({block: 'start'});
      this.focus_ = '';
    }
  }

  // Template helpers.

  protected isView_(view: View): boolean {
    return this.view_ === view;
  }

  protected getBackLabel_(): string {
    return this.view_ === 'site' ? 'Sites' : 'Privacy';
  }

  protected getSiteDataButtonText_(): string {
    return this.siteDataCleared_ ? 'Data Deleted' : 'Delete Site Data';
  }

  protected getBool_(name: string): boolean {
    return prefs.get(name)?.value === true;
  }

  protected getNumber_(name: string): number {
    const value = prefs.get(name)?.value;
    return typeof value === 'number' ? value : 0;
  }

  protected isEnforced_(name: string): boolean {
    return !!prefs.get(name)?.enforced;
  }

  protected getSafeBrowsingLevel_(): number {
    if (!this.getBool_(PREF_SAFE_BROWSING)) {
      return 0;
    }
    return this.getBool_(PREF_SAFE_BROWSING_ENHANCED) ? 2 : 1;
  }

  protected isSafeBrowsingEnforced_(): boolean {
    return this.isEnforced_(PREF_SAFE_BROWSING) ||
        this.isEnforced_(PREF_SAFE_BROWSING_ENHANCED);
  }

  protected getSecureDnsMode_(): string {
    const value = prefs.get(PREF_SECURE_DNS)?.value;
    return typeof value === 'string' ? value : 'off';
  }

  protected getSecureDnsDetail_(): string {
    return this.getSecureDnsMode_() === 'secure' ?
        'With the provider chosen in Chrome settings.' :
        'Looks up sites over an encrypted connection when your provider ' +
            'supports it.';
  }

  protected getPermissionLabel_(type: string): string {
    return PERMISSION_LABELS[type] ||
        type.replace(/-/g, ' ').replace(/^./, c => c.toUpperCase());
  }

  protected getValueLabel_(value: PermissionValue): string {
    switch (value) {
      case PermissionValue.kAllow:
        return 'Allow';
      case PermissionValue.kBlock:
        return 'Block';
      case PermissionValue.kAsk:
        return 'Ask';
      default:
        return 'Default';
    }
  }

  // A site's choice that follows the default says what that gives.
  protected getSiteValueLabel_(
      permission: SitePermission, value: PermissionValue): string {
    if (value !== PermissionValue.kDefault) {
      return this.getValueLabel_(value);
    }
    const effective = permission.value === PermissionValue.kDefault ?
        permission.effective :
        this.defaults_.find(d => d.type === permission.type)?.value;
    return effective === undefined ?
        'Default' :
        `Default (${this.getValueLabel_(effective)})`;
  }

  protected getSiteChoices_(permission: SitePermission): PermissionValue[] {
    return [PermissionValue.kDefault, ...permission.choices];
  }

  protected getSitesDetail_(): string {
    const count = this.sites_.length;
    return count === 1 ? '1 site' : `${count} sites`;
  }

  protected getPermissionCountText_(site: SiteSummary): string {
    return site.permissionCount === 1 ?
        '1 permission' :
        `${site.permissionCount} permissions`;
  }

  protected isClearChecked_(choice: ClearChoice): boolean {
    return !!this.clearChoices_[choice.key];
  }

  protected canClear_(): boolean {
    return !this.clearing_ && Object.values(this.clearChoices_).some(v => v);
  }

  protected getClearButtonText_(): string {
    if (this.clearing_) {
      return 'Clearing…';
    }
    return this.cleared_ ? 'Cleared' : 'Clear Data';
  }

  // Events.

  protected onBackClick_() {
    this.setView_(this.view_ === 'site' ? 'sites' : 'main');
  }

  // A row that opens a view (data-view).
  protected onViewClick_(e: Event) {
    this.setView_((e.currentTarget as HTMLElement).dataset['view'] as View);
  }

  protected onSiteClick_(e: Event) {
    this.openSite_((e.currentTarget as HTMLElement).dataset['origin']!);
  }

  protected onSwitchChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    prefs.set(input.dataset['pref']!, input.checked);
  }

  protected onCookieModeChange_(e: Event) {
    prefs.set(PREF_COOKIE_CONTROLS, Number((e.target as HTMLSelectElement).value));
  }

  protected onSafeBrowsingChange_(e: Event) {
    this.handler_.setSafeBrowsingLevel(
        Number((e.target as HTMLSelectElement).value));
  }

  protected onSecureDnsChange_(e: Event) {
    prefs.set(
        PREF_SECURE_DNS,
        (e.target as HTMLInputElement).checked ? 'automatic' : 'off');
  }

  protected onCertificatesClick_() {
    this.handler_.openUrl('chrome://certificate-manager/');
  }

  // Upstream's sections the pane doesn't rebuild yet (data-sub-page).
  protected onChromeSettingsClick_(e: Event) {
    this.handler_.openChromeSettings(
        (e.currentTarget as HTMLElement).dataset['subPage']!);
  }

  protected onDefaultChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    this.handler_.setPermissionDefault(
        select.dataset['type']!, Number(select.value) as PermissionValue);
  }

  protected onSitePermissionChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    this.handler_.setSitePermission(
        this.site_, select.dataset['type']!,
        Number(select.value) as PermissionValue);
  }

  protected onResetSiteClick_() {
    this.handler_.resetSitePermissions(this.site_);
  }

  protected async onClearSiteDataClick_() {
    await this.handler_.clearSiteData(this.site_);
    this.siteDataCleared_ = true;
  }

  protected onClearChoiceChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    this.clearChoices_ = {
      ...this.clearChoices_,
      [input.dataset['key']!]: input.checked,
    };
    this.cleared_ = false;
  }

  protected onClearPeriodChange_(e: Event) {
    this.clearPeriod_ = Number((e.target as HTMLSelectElement).value);
    this.cleared_ = false;
  }

  protected async onClearClick_() {
    this.clearing_ = true;
    // Kept for next time, as upstream's dialog keeps them.
    prefs.set(PREF_CLEAR_PERIOD, this.clearPeriod_);
    for (const choice of CLEAR_CHOICES) {
      prefs.set(choice.pref, !!this.clearChoices_[choice.key]);
    }
    const chosen = (key: ClearChoice['key']) => !!this.clearChoices_[key];
    await this.handler_.clearBrowsingData({
      timePeriod: this.clearPeriod_,
      history: chosen('history'),
      downloads: chosen('downloads'),
      cookies: chosen('cookies'),
      cache: chosen('cache'),
      passwords: chosen('passwords'),
      formData: chosen('formData'),
      siteSettings: chosen('siteSettings'),
    });
    this.clearing_ = false;
    this.cleared_ = true;
  }

  // Views.

  private setView_(view: View) {
    this.view_ = view;
    this.cleared_ = false;
    this.siteDataCleared_ = false;
    if (view === 'clear') {
      this.resetClearChoices_();
    }
    this.loadView_();
    const query = view === 'main' ? '' :
        view === 'site'           ? `?site=${encodeURIComponent(this.site_)}` :
                                    `?view=${view}`;
    window.history.replaceState(null, '', '/privacy' + query);
    this.closest('#scroller')?.scrollTo(0, 0);
  }

  private openSite_(origin: string) {
    this.site_ = origin;
    this.siteName_ = '';
    this.sitePermissions_ = [];
    this.setView_('site');
  }

  private async loadView_() {
    if (this.view_ === 'permissions' || this.view_ === 'site') {
      const {defaults} = await this.handler_.getPermissionDefaults();
      this.defaults_ = [...defaults].sort(byPermissionOrder);
    }
    if (this.view_ === 'main' || this.view_ === 'sites') {
      const {sites} = await this.handler_.getSites();
      this.sites_ = sites;
    }
    if (this.view_ === 'site') {
      const {displayName, permissions} =
          await this.handler_.getSitePermissions(this.site_);
      this.siteName_ = displayName || this.site_;
      this.sitePermissions_ = [...permissions].sort(byPermissionOrder);
    }
  }

  private onContentSettingsChanged_() {
    this.loadView_();
  }

  private resetClearChoices_() {
    const choices: Record<string, boolean> = {};
    for (const choice of CLEAR_CHOICES) {
      choices[choice.key] = this.getBool_(choice.pref);
    }
    this.clearChoices_ = choices;
    const period = prefs.get(PREF_CLEAR_PERIOD)?.value;
    this.clearPeriod_ = typeof period === 'number' ? period : 4;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-privacy-pane': CompassSettingsPrivacyPaneElement;
  }
}

customElements.define(
    CompassSettingsPrivacyPaneElement.is, CompassSettingsPrivacyPaneElement);
