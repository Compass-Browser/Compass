// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';
import type {AboutInfo} from '../sidebar.mojom-webui.js';
import {UpdateState} from '../sidebar.mojom-webui.js';

import {getHtml} from './about_pane.html.js';
import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';

export const PREF_UPDATE_CHANNEL = 'compass.update_channel';

export interface Choice<T> {
  value: T;
  label: string;
}

export const UPDATE_CHANNELS: Array<Choice<string>> = [
  {value: 'stable', label: i18n('aboutPaneChannelStable')},
  {value: 'beta', label: i18n('aboutPaneChannelBeta')},
  {value: 'canary', label: i18n('aboutPaneChannelCanary')},
];

// About (docs/design/settings-window.md): Compass's icon, name and version,
// its updates and update channel, and the open-source licenses. The last
// pane, and the one chrome://settings/help shows.
export class CompassSettingsAboutPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-about-pane';
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
    };
  }

  protected accessor about_: AboutInfo|null = null;
  // Null until a check starts.
  protected accessor updateState_: UpdateState|null = null;
  protected accessor updateProgress_: number = -1;
  protected accessor updateMessage_: string = '';
  protected accessor prefsVersion_: number = 0;

  protected readonly updateChannels = UPDATE_CHANNELS;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  private listenerId_: number|null = null;
  // What the path asked to scroll to ("updates", "channel"...).
  private focus_ = '';

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.focus_ =
        new URLSearchParams(window.location.search).get('focus') || '';
    prefs.addListener(this.prefListener_);
    prefs.load([PREF_UPDATE_CHANNEL]).then(() => this.prefsVersion_++);
    this.handler_.getAbout().then(
        ({about}: {about: AboutInfo}) => this.about_ = about);
    this.listenerId_ =
        SettingsWindowBrowserProxy.getInstance()
            .callbackRouter.updateStatusChanged.addListener(
                (state: UpdateState, progress: number, message: string) => {
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

  protected getString_(name: string): string {
    const value = prefs.get(name)?.value;
    return typeof value === 'string' ? value : '';
  }

  protected isEnforced_(name: string): boolean {
    return !!prefs.get(name)?.enforced;
  }

  protected getChannelLabel_(): string {
    const channel = this.getString_(PREF_UPDATE_CHANNEL) || 'stable';
    return UPDATE_CHANNELS.find(c => c.value === channel)?.label || '';
  }

  // "Version 20261125.1200 · Stable", or "Development build · Stable".
  protected getVersionText_(): string {
    if (!this.about_) {
      return '';
    }
    const channel = this.getChannelLabel_();
    // Development builds have no version of their own.
    const version = this.about_.compassVersion ?
        i18nF('aboutPaneVersion', this.about_.compassVersion) :
        i18n('aboutPaneDevelopmentBuild');
    return version + (channel ? ` · ${channel}` : '');
  }

  protected getChromiumText_(): string {
    return this.about_ ? `Chromium ${this.about_.chromiumVersion}` : '';
  }

  protected getUpdateLabel_(): string {
    if (this.about_ && !this.about_.canUpdate) {
      return i18n('aboutPaneUpdatesOff');
    }
    if (this.isUpdateReady_()) {
      return i18n('aboutPaneUpdateReady');
    }
    switch (this.updateState_) {
      case UpdateState.kChecking:
        return i18n('aboutPaneChecking');
      case UpdateState.kUpdating:
        return i18n('aboutPaneDownloading');
      case UpdateState.kUpToDate:
        return i18n('aboutPaneUpToDate');
      case UpdateState.kFailed:
        return i18n('aboutPaneCheckFailed');
      default:
        return i18n('aboutPaneUpdates');
    }
  }

  protected getUpdateText_(): string {
    if (this.about_ && !this.about_.canUpdate) {
      return i18n('aboutPaneUpdatesOffDetail');
    }
    if (this.about_?.pendingVersion && this.updateState_ === null) {
      return i18nF('aboutPaneVersionReady', this.about_.pendingVersion);
    }
    switch (this.updateState_) {
      case UpdateState.kChecking:
        return i18n('aboutPaneChecking');
      case UpdateState.kUpdating:
        return this.updateProgress_ >= 0 ?
            i18nF('aboutPaneDownloadingProgress', this.updateProgress_) :
            i18n('aboutPaneDownloading');
      case UpdateState.kReady:
        return i18n('aboutPaneUpdateReadyDetail');
      case UpdateState.kUpToDate:
        return i18n('aboutPaneUpToDateDetail');
      case UpdateState.kFailed:
        return this.updateMessage_ || i18n('aboutPaneCheckFailedDetail');
      default:
        return i18n('aboutPaneAutoUpdateDetail');
    }
  }

  // The update row's tile: green when there's nothing to do, the accent's
  // blue while there's something to install.
  protected getUpdateColor_(): string {
    if (this.updateState_ === UpdateState.kFailed) {
      return 'red';
    }
    return this.isUpdateReady_() ? 'blue' : 'green';
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
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-about-pane': CompassSettingsAboutPaneElement;
  }
}

customElements.define(
    CompassSettingsAboutPaneElement.is, CompassSettingsAboutPaneElement);
