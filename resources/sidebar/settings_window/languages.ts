// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {LanguageInfo, PreferredLanguage} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './languages.html.js';

// General › Languages (docs/design/settings-window.md, phase 5c): the
// preferred languages in order (move, remove, whether to offer translating
// pages in each), and the languages to add. As TranslatePrefs keeps them
// (intl.accept_languages, translate_blocked_languages). Fires "close" for
// the way back.
export class CompassSettingsLanguagesElement extends CrLitElement {
  static get is() {
    return 'compass-settings-languages';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      catalog_: {type: Array},
      preferred_: {type: Array},
      filter_: {type: String},
    };
  }

  protected accessor catalog_: LanguageInfo[] = [];
  protected accessor preferred_: PreferredLanguage[] = [];
  protected accessor filter_: string = '';

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private listenerId_: number|null = null;

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.listenerId_ = SettingsWindowBrowserProxy.getInstance()
                           .callbackRouter.languagesChanged.addListener(
                               () => this.load_());
    this.load_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.listenerId_ !== null) {
      SettingsWindowBrowserProxy.getInstance().callbackRouter.removeListener(
          this.listenerId_);
      this.listenerId_ = null;
    }
  }

  // Template helpers.

  protected getInfo_(code: string): LanguageInfo|undefined {
    return this.catalog_.find(l => l.code === code);
  }

  protected getName_(code: string): string {
    return this.getInfo_(code)?.name || code;
  }

  // Its own name, when that differs ("Italiano" for Italian).
  protected getNativeName_(code: string): string {
    const info = this.getInfo_(code);
    return info && info.nativeName !== info.name ? info.nativeName : '';
  }

  protected supportsTranslate_(code: string): boolean {
    return !!this.getInfo_(code)?.supportsTranslate;
  }

  protected isFirst_(index: number): boolean {
    return index === 0;
  }

  protected isLast_(index: number): boolean {
    return index === this.preferred_.length - 1;
  }

  protected canRemove_(): boolean {
    return this.preferred_.length > 1;
  }

  // The languages not preferred yet, matching the filter.
  protected getAddable_(): LanguageInfo[] {
    const filter = this.filter_.trim().toLowerCase();
    const preferred = new Set(this.preferred_.map(p => p.code));
    return this.catalog_.filter(
        l => !preferred.has(l.code) &&
            (!filter || l.name.toLowerCase().includes(filter) ||
             l.nativeName.toLowerCase().includes(filter) ||
             l.code.toLowerCase().startsWith(filter)));
  }

  // Events.

  protected onBackClick_() {
    this.fire('close');
  }

  protected onMoveUpClick_(e: Event) {
    this.handler_.moveLanguage(
        (e.currentTarget as HTMLElement).dataset['code']!, true);
  }

  protected onMoveDownClick_(e: Event) {
    this.handler_.moveLanguage(
        (e.currentTarget as HTMLElement).dataset['code']!, false);
  }

  protected onRemoveClick_(e: Event) {
    this.handler_.removeLanguage(
        (e.currentTarget as HTMLElement).dataset['code']!);
  }

  protected onTranslateChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    this.handler_.setTranslateOffered(input.dataset['code']!, input.checked);
  }

  protected onFilterInput_(e: Event) {
    this.filter_ = (e.target as HTMLInputElement).value;
  }

  protected onAddClick_(e: Event) {
    this.handler_.addLanguage(
        (e.currentTarget as HTMLElement).dataset['code']!);
  }

  private async load_() {
    const {catalog, preferred} = await this.handler_.getLanguages();
    this.catalog_ = catalog;
    this.preferred_ = preferred;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-languages': CompassSettingsLanguagesElement;
  }
}

customElements.define(
    CompassSettingsLanguagesElement.is, CompassSettingsLanguagesElement);
