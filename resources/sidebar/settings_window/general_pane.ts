// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';
import './languages.js';
import './search_engines.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';

// [font name, display name].
export type FontEntry = string[];
import {getHtml} from './general_pane.html.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';
import type {DefaultBrowserState, SearchEngine, SearchEngines} from '../sidebar.mojom-webui.js';

export const PREF_PROMPT_FOR_DOWNLOAD = 'download.prompt_for_download';
export const PREF_DOWNLOAD_DIRECTORY = 'download.default_directory';
export const PREF_FONT_SIZE = 'webkit.webprefs.default_font_size';
export const PREF_MINIMUM_FONT_SIZE = 'webkit.webprefs.minimum_font_size';
export const PREF_ACCEPT_LANGUAGES = 'intl.accept_languages';
export const PREF_OFFER_TRANSLATE = 'translate.enabled';
export const PREF_SPELLCHECK = 'browser.enable_spellchecking';
export const PREF_HARDWARE_ACCELERATION = 'hardware_acceleration_mode.enabled';

export type GeneralView = 'main'|'engines'|'languages';

export interface FontPref {
  pref: string;
  label: string;
}

export const FONT_PREFS: FontPref[] = [
  {pref: 'webkit.webprefs.fonts.standard.Zyyy', label: 'Standard font'},
  {pref: 'webkit.webprefs.fonts.serif.Zyyy', label: 'Serif font'},
  {pref: 'webkit.webprefs.fonts.sansserif.Zyyy', label: 'Sans-serif font'},
  {pref: 'webkit.webprefs.fonts.fixed.Zyyy', label: 'Fixed-width font'},
];

const ALL_PREFS = [
  PREF_PROMPT_FOR_DOWNLOAD,
  PREF_DOWNLOAD_DIRECTORY,
  PREF_FONT_SIZE,
  PREF_MINIMUM_FONT_SIZE,
  PREF_ACCEPT_LANGUAGES,
  PREF_OFFER_TRANSLATE,
  PREF_SPELLCHECK,
  PREF_HARDWARE_ACCELERATION,
  ...FONT_PREFS.map(f => f.pref),
];

export interface Choice {
  value: number;
  label: string;
}

// Upstream's font sizes (appearance_page): the default font's size.
export const FONT_SIZES: Choice[] = [
  {value: 9, label: 'Very small'},
  {value: 12, label: 'Small'},
  {value: 16, label: 'Medium'},
  {value: 20, label: 'Large'},
  {value: 24, label: 'Very large'},
];

export const MINIMUM_FONT_SIZES: Choice[] =
    [0, 6, 8, 10, 12, 14, 16, 18, 20, 24].map(
        size => ({value: size, label: size ? `${size} pt` : 'None'}));

// Upstream's zoom levels (as factors).
export const ZOOM_FACTORS: number[] =
    [0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2, 2.5,
     3, 4, 5];

// General (docs/design/settings-window.md, phase 2): the default browser,
// the search engine, downloads, appearance and fonts, languages and the
// system, with a view of its own for managing search engines and site
// search, on the window's handler (SettingsWindowHandler, which does what
// upstream's settings handlers do) and its prefs (prefs.ts). Managing search engines and editing the languages list
// still open upstream's sections.
export class CompassSettingsGeneralPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-general-pane';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      defaultBrowser_: {type: Object},
      engines_: {type: Array},
      enginesManaged_: {type: Boolean},
      downloadLocation_: {type: String},
      fonts_: {type: Array},
      zoom_: {type: Number},
      prefsVersion_: {type: Number},
      hardwareAccelerationAtStart_: {type: Boolean},
      view_: {type: String},
    };
  }

  protected accessor defaultBrowser_: DefaultBrowserState|null = null;
  protected accessor engines_: SearchEngine[] = [];
  protected accessor enginesManaged_: boolean = false;
  protected accessor downloadLocation_: string = '';
  protected accessor fonts_: FontEntry[] = [];
  protected accessor zoom_: number = 1;
  // Bumped as prefs change, to render again.
  protected accessor prefsVersion_: number = 0;
  protected accessor hardwareAccelerationAtStart_: boolean|null = null;
  protected accessor view_: GeneralView = 'main';
  // What the path asked to scroll to.
  private focus_ = '';

  protected readonly fontPrefs = FONT_PREFS;
  protected readonly fontSizes = FONT_SIZES;
  protected readonly minimumFontSizes = MINIMUM_FONT_SIZES;
  protected readonly zoomFactors = ZOOM_FACTORS;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (name: string, _pref: PrefValue) =>
      this.onPrefChanged_(name);

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    // ?view=engines (chrome://settings/searchEngines), ?focus=<row>.
    const params = new URLSearchParams(window.location.search);
    this.focus_ = params.get('focus') || '';
    const view = params.get('view');
    if (view === 'engines' || view === 'languages') {
      this.setView_(view);
    }
    prefs.addListener(this.prefListener_);
    prefs.load(ALL_PREFS).then(() => {
      if (this.hardwareAccelerationAtStart_ === null) {
        this.hardwareAccelerationAtStart_ =
            this.getBool_(PREF_HARDWARE_ACCELERATION);
      }
      this.updateDownloadLocation_();
      this.prefsVersion_++;
    });
    const router = SettingsWindowBrowserProxy.getInstance().callbackRouter;
    this.handler_.getDefaultBrowser().then(
        ({state}: {state: DefaultBrowserState}) => this.defaultBrowser_ = state);
    router.defaultBrowserChanged.addListener(
        (state: DefaultBrowserState) => this.defaultBrowser_ = state);
    this.handler_.getSearchEngines().then(
        ({engines}: {engines: SearchEngines}) => this.setSearchEngines_(engines));
    router.searchEnginesChanged.addListener(
        (engines: SearchEngines) => this.setSearchEngines_(engines));
    this.handler_.getFonts().then(
        ({fonts}: {fonts: string[][]}) => this.fonts_ = fonts);
    this.handler_.getDefaultZoom().then(
        ({factor}: {factor: number}) => this.zoom_ = factor);
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    prefs.removeListener(this.prefListener_);
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

  protected isView_(view: GeneralView): boolean {
    return this.view_ === view;
  }

  protected getBool_(name: string): boolean {
    return prefs.get(name)?.value === true;
  }

  protected getNumber_(name: string): number {
    const value = prefs.get(name)?.value;
    return typeof value === 'number' ? value : 0;
  }

  protected getString_(name: string): string {
    const value = prefs.get(name)?.value;
    return typeof value === 'string' ? value : '';
  }

  protected isEnforced_(name: string): boolean {
    return !!prefs.get(name)?.enforced;
  }

  protected getDefaultBrowserText_(): string {
    const info = this.defaultBrowser_;
    if (!info) {
      return '';
    }
    if (info.disabledByPolicy) {
      return 'Your organization manages the default browser.';
    }
    if (info.isDefault) {
      return 'Compass is your default browser.';
    }
    if (info.unknown || !info.canBeDefault) {
      return 'Compass can\'t tell or change the default browser here.';
    }
    return 'Compass isn\'t your default browser.';
  }

  protected canMakeDefault_(): boolean {
    const info = this.defaultBrowser_;
    return !!info && !info.isDefault && info.canBeDefault &&
        !info.disabledByPolicy && !info.unknown;
  }

  protected getDefaultEngineId_(): string {
    return this.engines_.find(e => e.isDefault)?.id || '';
  }

  protected isSearchEngineManaged_(): boolean {
    return this.enginesManaged_;
  }

  protected getZoomLabel_(factor: number): string {
    return `${Math.round(factor * 100)}%`;
  }

  // The zoom levels, with the current one if it isn't among them.
  protected getZoomFactors_(): number[] {
    const factors = [...ZOOM_FACTORS];
    if (!factors.some(f => Math.abs(f - this.zoom_) < 0.001)) {
      factors.push(this.zoom_);
      factors.sort((a, b) => a - b);
    }
    return factors;
  }

  protected isZoomSelected_(factor: number): boolean {
    return Math.abs(factor - this.zoom_) < 0.001;
  }

  // The fonts to choose from, with the pref's own if the list lacks it.
  protected getFontsFor_(pref: string): FontEntry[] {
    const current = this.getString_(pref);
    if (!current || this.fonts_.some(f => f[0] === current)) {
      return this.fonts_;
    }
    return [[current, current], ...this.fonts_];
  }

  // The languages, by name ("English (United States), Italian").
  protected getLanguagesText_(): string {
    const codes = this.getString_(PREF_ACCEPT_LANGUAGES)
                      .split(',')
                      .map(c => c.trim())
                      .filter(c => c);
    if (!codes.length) {
      return '';
    }
    let names: Intl.DisplayNames|null = null;
    try {
      names = new Intl.DisplayNames([navigator.language], {type: 'language'});
    } catch {
      // Codes, then.
    }
    return codes.map(c => names?.of(c) || c).join(', ');
  }

  protected needsRelaunch_(): boolean {
    return this.hardwareAccelerationAtStart_ !== null &&
        this.getBool_(PREF_HARDWARE_ACCELERATION) !==
        this.hardwareAccelerationAtStart_;
  }

  // Events.

  protected onMakeDefaultClick_() {
    this.handler_.setAsDefaultBrowser();
  }

  protected onSearchEngineChange_(e: Event) {
    this.handler_.setDefaultSearchEngine((e.target as HTMLSelectElement).value);
  }

  protected onChangeDownloadLocationClick_() {
    this.handler_.chooseDownloadLocation();
  }

  // A switch bound to a boolean pref (data-pref).
  protected onSwitchChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    prefs.set(input.dataset['pref']!, input.checked);
  }

  // A select bound to a number pref (data-pref).
  protected onNumberSelectChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    prefs.set(select.dataset['pref']!, Number(select.value));
  }

  // A select bound to a string pref (data-pref).
  protected onFontChange_(e: Event) {
    const select = e.target as HTMLSelectElement;
    prefs.set(select.dataset['pref']!, select.value);
  }

  protected onZoomChange_(e: Event) {
    this.zoom_ = Number((e.target as HTMLSelectElement).value);
    this.handler_.setDefaultZoom(this.zoom_);
  }

  protected onRelaunchClick_() {
    this.handler_.relaunch();
  }

  protected onProxyClick_() {
    this.handler_.showProxySettings();
  }

  // Manage search engines.

  protected onManageEnginesClick_() {
    this.setView_('engines');
  }

  protected onEnginesClose_() {
    this.setView_('main');
  }

  protected onManageLanguagesClick_() {
    this.setView_('languages');
  }

  protected onLanguagesClose_() {
    this.setView_('main');
  }

  private setView_(view: GeneralView) {
    this.view_ = view;
    window.history.replaceState(
        null, '', view === 'main' ? '/general' : `/general?view=${view}`);
    this.closest('#scroller')?.scrollTo(0, 0);
  }

  private onPrefChanged_(name: string) {
    if (name === PREF_DOWNLOAD_DIRECTORY) {
      this.updateDownloadLocation_();
    }
    this.prefsVersion_++;
  }

  private async updateDownloadLocation_() {
    const {location} = await this.handler_.getDownloadLocation();
    this.downloadLocation_ = location;
  }

  // The ones that can be the default, as the handler lists them.
  private setSearchEngines_(engines: SearchEngines) {
    this.engines_ = engines.engines;
    this.enginesManaged_ = engines.managed;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-general-pane': CompassSettingsGeneralPaneElement;
  }
}

customElements.define(
    CompassSettingsGeneralPaneElement.is, CompassSettingsGeneralPaneElement);
