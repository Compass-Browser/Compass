// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {getHtml} from './advanced_pane.html.js';
import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import type {PrefValue} from './prefs.js';
import {prefs} from './prefs.js';

export const PREF_MEMORY_SAVER = 'performance_tuning.high_efficiency_mode.state';
export const PREF_ENERGY_SAVER = 'performance_tuning.battery_saver_mode.state';
export const PREF_LIVE_CAPTION = 'accessibility.captions.live_caption_enabled';
export const PREF_FOCUS_HIGHLIGHT = 'settings.a11y.focus_highlight';
export const PREF_CARET_BROWSING = 'settings.a11y.caretbrowsing.enabled';

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

// Advanced (docs/design/settings-window.md, phase 5): performance,
// accessibility, and resetting upstream's settings. Development Mode is a row
// under it that opens its own page (panes.ts); Compass's version and updates
// are the About pane's (about_pane.ts).
export class CompassSettingsAdvancedPaneElement extends CrLitElement {
  static get is() {
    return 'compass-settings-advanced-pane';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      prefsVersion_: {type: Number},
      confirmingReset_: {type: Boolean},
      resetting_: {type: Boolean},
      resetDone_: {type: Boolean},
    };
  }

  protected accessor prefsVersion_: number = 0;
  protected accessor confirmingReset_: boolean = false;
  protected accessor resetting_: boolean = false;
  protected accessor resetDone_: boolean = false;

  protected readonly energySaverModes = ENERGY_SAVER_MODES;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private prefListener_ = (_name: string, _pref: PrefValue) =>
      this.prefsVersion_++;
  // What the path asked to scroll to ("performance", "reset"...).
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
         ])
        .then(() => this.prefsVersion_++);
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    prefs.removeListener(this.prefListener_);
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

  protected isEnforced_(name: string): boolean {
    return !!prefs.get(name)?.enforced;
  }

  protected isMemorySaverOn_(): boolean {
    return this.getNumber_(PREF_MEMORY_SAVER) === 2;
  }

  protected getResetText_(): string {
    if (this.resetting_) {
      return 'Resetting…';
    }
    return this.resetDone_ ? 'Settings were reset.' :
                             'Search engine, permissions, startup and more.';
  }

  // Events.

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
