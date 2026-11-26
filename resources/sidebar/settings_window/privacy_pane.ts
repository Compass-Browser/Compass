// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF, i18nPlural} from '../i18n.js';
import type {FilterList, PermissionDefault, SafetyCheckResult, SitePermission, SiteSummary} from '../sidebar.mojom-webui.js';
import {PermissionValue, UpdateState} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';
import {getHtml} from './privacy_pane.html.js';

export const PREF_COOKIE_CONTROLS = 'profile.cookie_controls_mode';
export const PREF_DO_NOT_TRACK = 'enable_do_not_track';
export const PREF_HTTPS_ONLY = 'https_only_mode_enabled';
export const PREF_SECURE_DNS = 'dns_over_https.mode';
export const PREF_CLEAN_LINKS = 'compass.copy.remove_tracking';
export const PREF_ADBLOCK = 'compass.adblock.enabled';
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
    label: i18n('privacyPaneClearHistory'),
    detail: i18n('privacyPaneClearHistoryDetail'),
  },
  {
    key: 'downloads',
    pref: 'browser.clear_data.download_history',
    label: i18n('privacyPaneClearDownloads'),
    detail: i18n('privacyPaneClearDownloadsDetail'),
  },
  {
    key: 'cookies',
    pref: 'browser.clear_data.cookies',
    label: i18n('privacyPaneClearCookies'),
    detail: i18n('privacyPaneClearCookiesDetail'),
  },
  {
    key: 'cache',
    pref: 'browser.clear_data.cache',
    label: i18n('privacyPaneClearCache'),
    detail: i18n('privacyPaneClearCacheDetail'),
  },
  {
    key: 'passwords',
    pref: 'browser.clear_data.passwords',
    label: i18n('privacyPaneClearPasswords'),
    detail: i18n('privacyPaneClearPasswordsDetail'),
  },
  {
    key: 'formData',
    pref: 'browser.clear_data.form_data',
    label: i18n('privacyPaneClearFormData'),
    detail: i18n('privacyPaneClearFormDataDetail'),
  },
  {
    key: 'siteSettings',
    pref: 'browser.clear_data.site_settings',
    label: i18n('privacyPaneClearSiteSettings'),
    detail: i18n('privacyPaneClearSiteSettingsDetail'),
  },
];

export interface Choice {
  value: number;
  label: string;
}

// browsing_data::TimePeriod.
export const TIME_PERIODS: Choice[] = [
  {value: 6, label: i18n('privacyPaneLast15Minutes')},
  {value: 0, label: i18n('privacyPaneLastHour')},
  {value: 1, label: i18n('privacyPaneLast24Hours')},
  {value: 2, label: i18n('privacyPaneLast7Days')},
  {value: 3, label: i18n('privacyPaneLast4Weeks')},
  {value: 4, label: i18n('privacyPaneAllTime')},
];

// content_settings::CookieControlsMode.
export const COOKIE_MODES: Choice[] = [
  {value: 0, label: i18n('privacyPaneAllow')},
  {value: 2, label: i18n('privacyPaneCookiesBlockIncognito')},
  {value: 1, label: i18n('privacyPaneBlock')},
];

// safe_browsing::SafeBrowsingState.
export const SAFE_BROWSING_LEVELS: Choice[] = [
  {value: 2, label: i18n('privacyPaneSafeBrowsingEnhanced')},
  {value: 1, label: i18n('privacyPaneSafeBrowsingStandard')},
  {value: 0, label: i18n('privacyPaneSafeBrowsingOff')},
];

// The permissions by upstream's group names, as the window names them.
const PERMISSION_LABELS: Record<string, string> = {
  'location': i18n('privacyPanePermissionLocation'),
  'media-stream-camera': i18n('privacyPanePermissionMediaStreamCamera'),
  'media-stream-mic': i18n('privacyPanePermissionMediaStreamMic'),
  'notifications': i18n('privacyPanePermissionNotifications'),
  'popups': i18n('privacyPanePermissionPopups'),
  'javascript': i18n('privacyPanePermissionJavascript'),
  'javascript-optimizer': i18n('privacyPanePermissionJavascriptOptimizer'),
  'images': i18n('privacyPanePermissionImages'),
  'sound': i18n('privacyPanePermissionSound'),
  'multiple-automatic-downloads':
      i18n('privacyPanePermissionMultipleAutomaticDownloads'),
  'clipboard': i18n('privacyPanePermissionClipboard'),
  'sensors': i18n('privacyPanePermissionSensors'),
  'midi-sysex': i18n('privacyPanePermissionMidiSysex'),
  'usb-devices': i18n('privacyPanePermissionUsbDevices'),
  'serial-ports': i18n('privacyPanePermissionSerialPorts'),
  'hid-devices': i18n('privacyPanePermissionHidDevices'),
  'bluetooth-scanning': i18n('privacyPanePermissionBluetoothScanning'),
  'file-system-write': i18n('privacyPanePermissionFileSystemWrite'),
  'window-management': i18n('privacyPanePermissionWindowManagement'),
  'local-fonts': i18n('privacyPanePermissionLocalFonts'),
  'protected-content': i18n('privacyPanePermissionProtectedContent'),
  'mixed-script': i18n('privacyPanePermissionMixedScript'),
  'background-sync': i18n('privacyPanePermissionBackgroundSync'),
  'payment-handler': i18n('privacyPanePermissionPaymentHandler'),
  'idle-detection': i18n('privacyPanePermissionIdleDetection'),
  'storage-access': i18n('privacyPanePermissionStorageAccess'),
  'federated-identity-api': i18n('privacyPanePermissionFederatedIdentityApi'),
  'auto-picture-in-picture': i18n('privacyPanePermissionAutoPictureInPicture'),
  // Blocked: the ad blocker is on for the site (docs/design/ad-blocker.md).
  'ads': i18n('privacyPanePermissionAds'),
  'vr': i18n('privacyPanePermissionVr'),
  'ar': i18n('privacyPanePermissionAr'),
  'smart-card-readers': i18n('privacyPanePermissionSmartCardReaders'),
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

export type View =
    'main'|'clear'|'permissions'|'sites'|'site'|'safety'|'adblock';

// "3 hours ago", for a list's last update.
function timeAgo(ms: number): string {
  const minutes = Math.round((Date.now() - ms) / 60000);
  if (minutes < 1) {
    return i18n('privacyPaneJustNow');
  }
  if (minutes < 60) {
    return i18nPlural('privacyPaneMinutesAgo', minutes);
  }
  const hours = Math.round(minutes / 60);
  if (hours < 24) {
    return i18nPlural('privacyPaneHoursAgo', hours);
  }
  const days = Math.round(hours / 24);
  return days === 1 ? i18n('privacyPaneYesterday') :
                      i18nPlural('privacyPaneDaysAgo', days);
}

// Privacy (docs/design/settings-window.md, phase 3): clearing browsing data,
// cookies, security, the ad blocker and its filter lists
// (docs/design/ad-blocker.md), and site settings (the permissions' defaults,
// the sites with permissions of their own, one site's) and Safety check,
// each a view of the pane with a way back.
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
      safety_: {type: Object},
      canUpdate_: {type: Boolean},
      updateState_: {type: Number},
      updateMessage_: {type: String},
      filterLists_: {type: Array},
      listUrl_: {type: String},
      listError_: {type: String},
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
  protected accessor safety_: SafetyCheckResult|null = null;
  protected accessor canUpdate_: boolean = false;
  protected accessor updateState_: UpdateState|null = null;
  protected accessor updateMessage_: string = '';
  protected accessor filterLists_: FilterList[] = [];
  protected accessor listUrl_: string = '';
  protected accessor listError_: string = '';

  protected readonly clearChoices = CLEAR_CHOICES;
  protected readonly timePeriods = TIME_PERIODS;
  protected readonly cookieModes = COOKIE_MODES;
  protected readonly safeBrowsingLevels = SAFE_BROWSING_LEVELS;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  private contentListenerId_: number|null = null;
  private updateListenerId_: number|null = null;
  private filterListsListenerId_: number|null = null;
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
    } else if (
        view === 'clear' || view === 'permissions' || view === 'sites' ||
        view === 'safety' || view === 'adblock') {
      this.setView_(view);
    } else {
      this.loadView_();
    }
    prefs.addListener(this.prefListener_);
    prefs
        .load([
          PREF_COOKIE_CONTROLS,
          PREF_DO_NOT_TRACK,
          PREF_HTTPS_ONLY,
          PREF_SECURE_DNS,
          PREF_CLEAN_LINKS,
          PREF_ADBLOCK,
          PREF_SAFE_BROWSING,
          PREF_SAFE_BROWSING_ENHANCED,
          PREF_CLEAR_PERIOD,
          ...CLEAR_CHOICES.map(c => c.pref),
        ])
        .then(() => {
          this.resetClearChoices_();
          this.prefsVersion_++;
        });
    const router = SettingsWindowBrowserProxy.getInstance().callbackRouter;
    this.contentListenerId_ = router.contentSettingsChanged.addListener(
        () => this.onContentSettingsChanged_());
    this.updateListenerId_ = router.updateStatusChanged.addListener(
        (state: UpdateState, _progress: number, message: string) => {
          this.updateState_ = state;
          this.updateMessage_ = message;
        });
    this.filterListsListenerId_ =
        router.filterListsChanged.addListener(() => this.loadFilterLists_());
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    prefs.removeListener(this.prefListener_);
    const router = SettingsWindowBrowserProxy.getInstance().callbackRouter;
    for (const id
             of [this.contentListenerId_, this.updateListenerId_,
                 this.filterListsListenerId_]) {
      if (id !== null) {
        router.removeListener(id);
      }
    }
    this.contentListenerId_ = null;
    this.updateListenerId_ = null;
    this.filterListsListenerId_ = null;
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

  // Safety check.

  protected getUpdatesText_(): string {
    if (!this.canUpdate_) {
      return i18n('aboutPaneUpdatesOffDetail');
    }
    switch (this.updateState_) {
      case UpdateState.kChecking:
      case UpdateState.kUpdating:
        return i18n('aboutPaneChecking');
      case UpdateState.kReady:
        return i18n('privacyPaneUpdateReady');
      case UpdateState.kUpToDate:
        return i18n('aboutPaneUpToDateDetail');
      case UpdateState.kFailed:
        return this.updateMessage_ || i18n('aboutPaneCheckFailedDetail');
      default:
        return i18n('aboutPaneAutoUpdateDetail');
    }
  }

  protected isUpdatesOk_(): boolean {
    return this.updateState_ === UpdateState.kUpToDate;
  }

  protected getPasswordsText_(): string {
    const safety = this.safety_;
    if (!safety) {
      return i18n('privacyPaneChecking');
    }
    const problems: string[] = [];
    if (safety.compromisedPasswords) {
      problems.push(
          i18nPlural('privacyPaneCompromised', safety.compromisedPasswords));
    }
    if (safety.weakPasswords) {
      problems.push(i18nPlural('privacyPaneWeak', safety.weakPasswords));
    }
    if (safety.reusedPasswords) {
      problems.push(i18nPlural('privacyPaneReused', safety.reusedPasswords));
    }
    if (problems.length) {
      return i18nF('privacyPanePasswordProblems', problems.join(', '));
    }
    return safety.savedPasswords ?
        i18nPlural('privacyPaneNoPasswordProblems', safety.savedPasswords) :
        i18n('privacyPaneNoSavedPasswords');
  }

  protected isPasswordsOk_(): boolean {
    const safety = this.safety_;
    return !!safety && !safety.compromisedPasswords && !safety.weakPasswords &&
        !safety.reusedPasswords;
  }

  protected getSafeBrowsingText_(): string {
    return this.getSafeBrowsingLevel_() ?
        i18n('privacyPaneSafeBrowsingOn') :
        i18n('privacyPaneSafeBrowsingOffDetail');
  }

  protected getExtensionsText_(): string {
    const safety = this.safety_;
    if (!safety) {
      return i18n('privacyPaneChecking');
    }
    if (safety.blocklistedExtensions) {
      return i18nPlural(
          'privacyPaneHarmfulExtensions', safety.blocklistedExtensions);
    }
    return safety.enabledExtensions ?
        i18nPlural('privacyPaneNoHarmfulExtensions', safety.enabledExtensions) :
        i18n('privacyPaneNoExtensions');
  }

  protected isExtensionsOk_(): boolean {
    return !!this.safety_ && !this.safety_.blocklistedExtensions;
  }

  // Ad blocker.

  protected getFilterListsDetail_(): string {
    const enabled = this.filterLists_.filter(list => list.enabled);
    const count = i18nPlural('privacyPaneListCount', enabled.length);
    const updated = Math.max(0, ...enabled.map(list => list.updatedMs));
    return updated ? i18nF('privacyPaneListsUpdated', count, timeAgo(updated)) :
                     count;
  }

  protected getFilterListDetail_(list: FilterList): string {
    if (list.updating) {
      return i18n('privacyPaneUpdating');
    }
    if (!list.updatedMs) {
      return list.error || i18n('privacyPaneNotDownloaded');
    }
    const rules = list.urlRules + list.cssRules;
    const parts = [
      i18nPlural('privacyPaneRuleCount', rules),
      i18nF('privacyPaneListUpdated', timeAgo(list.updatedMs)),
    ];
    if (list.skipped) {
      parts.push(i18nPlural('privacyPaneRulesNotSupported', list.skipped));
    }
    return parts.join(' · ');
  }

  protected isUpdatingLists_(): boolean {
    return this.filterLists_.some(list => list.updating);
  }

  protected onFilterListChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    this.handler_.setFilterListEnabled(input.dataset['id']!, input.checked);
  }

  protected onRemoveFilterListClick_(e: Event) {
    this.handler_.removeFilterList(
        (e.currentTarget as HTMLElement).dataset['id']!);
  }

  protected onListUrlInput_(e: Event) {
    this.listUrl_ = (e.target as HTMLInputElement).value;
    this.listError_ = '';
  }

  protected onListUrlKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      this.onAddFilterListClick_();
    }
  }

  protected async onAddFilterListClick_() {
    if (!this.listUrl_.trim()) {
      return;
    }
    const {error} = await this.handler_.addFilterList(this.listUrl_);
    this.listError_ = error;
    if (!error) {
      this.listUrl_ = '';
    }
  }

  protected onUpdateFilterListsClick_() {
    this.handler_.updateFilterListsNow();
  }

  protected getBackLabel_(): string {
    return this.view_ === 'site' ? i18n('privacyPaneSites') :
                                   i18n('settingsWindowPanePrivacy');
  }

  protected getSiteDataButtonText_(): string {
    return this.siteDataCleared_ ? i18n('privacyPaneSiteDataDeleted') :
                                   i18n('privacyPaneDeleteSiteData');
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
        i18n('privacyPaneSecureDnsCustom') :
        i18n('privacyPaneSecureDnsAutomatic');
  }

  protected getPermissionLabel_(type: string): string {
    return PERMISSION_LABELS[type] ||
        type.replace(/-/g, ' ').replace(/^./, c => c.toUpperCase());
  }

  protected getValueLabel_(value: PermissionValue): string {
    switch (value) {
      case PermissionValue.kAllow:
        return i18n('privacyPaneAllow');
      case PermissionValue.kBlock:
        return i18n('privacyPaneBlock');
      case PermissionValue.kAsk:
        return i18n('privacyPaneAsk');
      default:
        return i18n('privacyPaneDefault');
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
        i18n('privacyPaneDefault') :
        i18nF('privacyPaneDefaultWithValue', this.getValueLabel_(effective));
  }

  protected getSiteChoices_(permission: SitePermission): PermissionValue[] {
    return [PermissionValue.kDefault, ...permission.choices];
  }

  protected getSitesDetail_(): string {
    return i18nPlural('privacyPaneSiteCount', this.sites_.length);
  }

  // Safety check's row: how many sites have permissions of their own.
  protected getSitesWithOwnText_(): string {
    return i18nPlural('privacyPaneSitesWithOwnCount', this.sites_.length);
  }

  protected getPermissionCountText_(site: SiteSummary): string {
    return i18nPlural('privacyPanePermissionCount', site.permissionCount);
  }

  protected isClearChecked_(choice: ClearChoice): boolean {
    return !!this.clearChoices_[choice.key];
  }

  protected canClear_(): boolean {
    return !this.clearing_ && Object.values(this.clearChoices_).some(v => v);
  }

  protected getClearButtonText_(): string {
    if (this.clearing_) {
      return i18n('privacyPaneClearing');
    }
    return this.cleared_ ? i18n('privacyPaneCleared') :
                           i18n('privacyPaneClearData');
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
    prefs.set(
        PREF_COOKIE_CONTROLS, Number((e.target as HTMLSelectElement).value));
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

  protected onCheckUpdatesClick_() {
    this.updateState_ = UpdateState.kChecking;
    this.handler_.checkForUpdates();
  }

  protected onReviewPasswordsClick_() {
    this.handler_.openUrl('chrome://password-manager/checkup');
  }

  protected onTurnOnSafeBrowsingClick_() {
    this.handler_.setSafeBrowsingLevel(1);
  }

  protected onManageExtensionsClick_() {
    this.handler_.openUrl('chrome://extensions/');
  }

  protected onCertificatesClick_() {
    this.handler_.openUrl('chrome://certificate-manager/');
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
    this.listError_ = '';
    this.loadView_();
    const query = view === 'main' ? '' :
        view === 'site'           ? `?site=${encodeURIComponent(this.site_)}` :
                                    `?view=${view}`;
    window.history.replaceState(null, '', '/privacy' + query);
    // The window hides the pane's header while a view is shown (app.ts).
    this.toggleAttribute('nested', view !== 'main');
    this.dispatchEvent(new CustomEvent('settings-view', {bubbles: true}));
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
    if (this.view_ === 'main' || this.view_ === 'adblock') {
      this.loadFilterLists_();
    }
    if (this.view_ === 'safety') {
      const [{result}, {about}] = await Promise.all(
          [this.handler_.getSafetyCheck(), this.handler_.getAbout()]);
      this.safety_ = result;
      this.canUpdate_ = about.canUpdate;
    }
    if (this.view_ === 'site') {
      const {displayName, permissions} =
          await this.handler_.getSitePermissions(this.site_);
      this.siteName_ = displayName || this.site_;
      this.sitePermissions_ = [...permissions].sort(byPermissionOrder);
    }
  }

  private async loadFilterLists_() {
    const {lists} = await this.handler_.getFilterLists();
    this.filterLists_ = lists;
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
