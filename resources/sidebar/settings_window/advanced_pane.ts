// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import type {AboutInfo} from '../sidebar.mojom-webui.js';
import {UpdateState} from '../sidebar.mojom-webui.js';

import {getHtml} from './advanced_pane.html.js';
import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';

export const PREF_MEMORY_SAVER = 'performance_tuning.high_efficiency_mode.state';
export const PREF_ENERGY_SAVER = 'performance_tuning.battery_saver_mode.state';
export const PREF_LIVE_CAPTION = 'accessibility.captions.live_caption_enabled';
export const PREF_FOCUS_HIGHLIGHT = 'settings.a11y.focus_highlight';
export const PREF_CARET_BROWSING = 'settings.a11y.caretbrowsing.enabled';
export const PREF_UPDATE_CHANNEL = 'compass.update_channel';

export interface Choice<T> {
  value: T;
  label: string;
}

// performance_manager::user_tuning::prefs::BatterySaverModeState.
export const ENERGY_SAVER_MODES: Array<Choice<number>> = [
  {value: 0, label: 'Off'},
  {value: 1, label: 'When the battery is low'},
  {value: 2, label: 'When unplugged'},
  {value: 3, label: 'Always'},
];

export const UPDATE_CHANNELS: Array<Choice<string>> = [
  {value: 'stable', label: 'Stable'},
  {value: 'beta', label: 'Beta'},
  {value: 'canary', label: 'Canary'},
];

// Advanced (docs/design/settings-window.md, phase 5): About Compass (its
// version, updates and channel), performance, accessibility, and resetting
// upstream's settings. Development Mode follows it in the pane.
export class CompassSettingsAdvancedPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-advanced-pane';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      about_: {type: Object},
      updateState_: {type: Number},
      updateProgress_: {type: Number},
      updateMessage_: {type: String},
      prefsVersion_: {type: Number},
      confirmingReset_: {type: Boolean},
      resetting_: {type: Boolean},
      resetDone_: {type: Boolean},
    };
  }

  protected accessor about_: AboutInfo|null = null;
  // Null until a check starts.
  protected accessor updateState_: UpdateState|null = null;
  protected accessor updateProgress_: number = -1;
  protected accessor updateMessage_: string = '';
  protected accessor prefsVersion_: number = 0;
  protected accessor confirmingReset_: boolean = false;
  protected accessor resetting_: boolean = false;
  protected accessor resetDone_: boolean = false;

  protected readonly energySaverModes = ENERGY_SAVER_MODES;
  protected readonly updateChannels = UPDATE_CHANNELS;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  private listenerId_: number|null = null;
  // What the path asked to scroll to ("about", "performance"...).
  private focus_ = '';

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.focus_ = new URLSearchParams(window.location.search).get('focus') || '';
    prefs.addListener(this.prefListener_);
    prefs.load([
           PREF_MEMORY_SAVER,
           PREF_ENERGY_SAVER,
           PREF_LIVE_CAPTION,
           PREF_FOCUS_HIGHLIGHT,
           PREF_CARET_BROWSING,
           PREF_UPDATE_CHANNEL,
         ])
        .then(() => this.prefsVersion_++);
    this.handler_.getAbout().then(
        ({about}: {about: AboutInfo}) => this.about_ = about);
    this.listenerId_ = SettingsWindowBrowserProxy.getInstance()
                           .callbackRouter.updateStatusChanged.addListener(
                               (state: UpdateState, progress: number,
                                message: string) => {
                                 this.updateState_ = state;
                                 this.updateProgress_ = progress;
                                 this.updateMessage_ = message;
                               });
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    prefs.removeListener(this.prefListener_);
    if (this.listenerId_ !== null) {
      SettingsWindowBrowserProxy.getInstance().callbackRouter.removeListener(
          this.listenerId_);
      this.listenerId_ = null;
    }
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

  protected isMemorySaverOn_(): boolean {
    return this.getNumber_(PREF_MEMORY_SAVER) === 2;
  }

  protected getVersionText_(): string {
    if (!this.about_) {
      return '';
    }
    return `Version ${this.about_.compassVersion} (Chromium ` +
        `${this.about_.chromiumVersion})`;
  }

  protected getUpdateText_(): string {
    if (this.about_ && !this.about_.canUpdate) {
      return 'This build doesn\'t update itself.';
    }
    if (this.about_?.pendingVersion && this.updateState_ === null) {
      return `Version ${this.about_.pendingVersion} is ready to install.`;
    }
    switch (this.updateState_) {
      case UpdateState.kChecking:
        return 'Checking for updates…';
      case UpdateState.kUpdating:
        return this.updateProgress_ >= 0 ?
            `Downloading the update… ${this.updateProgress_}%` :
            'Downloading the update…';
      case UpdateState.kReady:
        return 'The update is ready to install.';
      case UpdateState.kUpToDate:
        return 'Compass is up to date.';
      case UpdateState.kFailed:
        return this.updateMessage_ || 'Couldn\'t check for updates.';
      default:
        return 'Compass checks for updates by itself.';
    }
  }

  protected isUpdateReady_(): boolean {
    return this.updateState_ === UpdateState.kReady ||
        (this.updateState_ === null && !!this.about_?.pendingVersion);
  }

  protected canCheck_(): boolean {
    return !!this.about_?.canUpdate && !this.isUpdateReady_() &&
        this.updateState_ !== UpdateState.kChecking &&
        this.updateState_ !== UpdateState.kUpdating;
  }

  protected getResetText_(): string {
    if (this.resetting_) {
      return 'Resetting…';
    }
    return this.resetDone_ ? 'Settings were reset.' :
                             'Search engine, permissions, startup and more.';
  }

  // Events.

  protected onCheckClick_() {
    this.updateState_ = UpdateState.kChecking;
    this.handler_.checkForUpdates();
  }

  protected onInstallClick_() {
    this.handler_.installUpdate();
  }

  // Compass's own (third_party) and Chromium's.
  protected onLicensesClick_() {
    this.handler_.openUrl('chrome://compass-settings/licenses.html');
  }

  protected onChannelChange_(e: Event) {
    prefs.set(PREF_UPDATE_CHANNEL, (e.target as HTMLSelectElement).value);
  }

  protected onMemorySaverChange_(e: Event) {
    prefs.set(PREF_MEMORY_SAVER, (e.target as HTMLInputElement).checked ? 2 : 0);
  }

  protected onEnergySaverChange_(e: Event) {
    prefs.set(PREF_ENERGY_SAVER, Number((e.target as HTMLSelectElement).value));
  }

  protected onSwitchChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    prefs.set(input.dataset['pref']!, input.checked);
  }

  protected onResetClick_() {
    this.confirmingReset_ = true;
    this.resetDone_ = false;
  }

  protected onResetCancelClick_() {
    this.confirmingReset_ = false;
  }

  protected async onResetConfirmClick_() {
    this.confirmingReset_ = false;
    this.resetting_ = true;
    await this.handler_.resetSettings();
    this.resetting_ = false;
    this.resetDone_ = true;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-advanced-pane': CompassSettingsAdvancedPaneElement;
  }
}

customElements.define(
    CompassSettingsAdvancedPaneElement.is, CompassSettingsAdvancedPaneElement);
