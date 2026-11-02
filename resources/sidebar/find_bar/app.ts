// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {ScrollKey} from '../find_bar.mojom-webui.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {FindBarBrowserProxy} from './browser_proxy.js';

// The find bar: a field, the match count, previous and next, close. Typing
// finds from the top of the page; Enter and Shift+Enter move between
// matches, Esc closes. Up, Down, Page Up and Page Down scroll the page. The
// browser (compass::FindBar) keeps each tab's text and pushes it on a tab
// switch. See docs/design/claude.md, "Find bar".
export class CompassFindBarAppElement extends CrLitElement {
  static get is() {
    return 'compass-find-bar-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      active_: {type: Number},
      count_: {type: Number},
      final_: {type: Boolean},
      text_: {type: String},
    };
  }

  // The result for the field's text; count -1 for none yet.
  protected accessor active_: number = 0;
  protected accessor count_: number = -1;
  protected accessor final_: boolean = true;
  protected accessor text_: string = '';

  private browserProxy_: FindBarBrowserProxy =
      FindBarBrowserProxy.getInstance();
  private listenerIds_: number[] = [];

  override connectedCallback() {
    super.connectedCallback();
    // Follows the window's theme, which follows its space.
    ColorChangeUpdater.forDocument().start();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.setText.addListener(this.onSetText_.bind(this)),
      router.setResult.addListener(this.onSetResult_.bind(this)),
      router.clearResult.addListener(this.onClearResult_.bind(this)),
      router.focusAndSelectAll.addListener(
          this.onFocusAndSelectAll_.bind(this)),
    ];
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    for (const id of this.listenerIds_) {
      this.browserProxy_.callbackRouter.removeListener(id);
    }
    this.listenerIds_ = [];
  }

  private get input_(): HTMLInputElement {
    return this.shadowRoot.querySelector<HTMLInputElement>('#input')!;
  }

  private async onSetText_(
      text: string, selectionStart: number, selectionEnd: number) {
    this.text_ = text;
    this.count_ = -1;
    await this.updateComplete;
    const input = this.input_;
    input.value = text;
    input.setSelectionRange(selectionStart, selectionEnd);
  }

  private onSetResult_(active: number, count: number, finalUpdate: boolean) {
    this.active_ = active;
    this.count_ = count;
    this.final_ = finalUpdate;
  }

  private onClearResult_() {
    this.count_ = -1;
  }

  private async onFocusAndSelectAll_() {
    await this.updateComplete;
    const input = this.input_;
    input.focus();
    input.select();
  }

  protected onInput_(e: InputEvent) {
    // A composition finds once it's committed (onCompositionEnd_).
    if (e.isComposing) {
      return;
    }
    this.find_();
  }

  protected onCompositionEnd_() {
    this.find_();
  }

  private find_() {
    const text = this.input_.value;
    if (text === this.text_) {
      return;
    }
    this.text_ = text;
    this.count_ = -1;
    this.browserProxy_.handler.find(text);
  }

  protected onKeydown_(e: KeyboardEvent) {
    if (e.isComposing) {
      return;
    }
    const scrollKeys: Record<string, ScrollKey> = {
      'ArrowUp': ScrollKey.kUp,
      'ArrowDown': ScrollKey.kDown,
      'PageUp': ScrollKey.kPageUp,
      'PageDown': ScrollKey.kPageDown,
    };
    switch (e.key) {
      case 'Enter':
        e.preventDefault();
        this.browserProxy_.handler.findNext(e.shiftKey);
        return;
      case 'Escape':
        e.preventDefault();
        this.browserProxy_.handler.close();
        return;
      case 'Home':
      case 'End':
        // Ctrl+Home/End scroll the page; alone they move the caret.
        if (e.ctrlKey) {
          e.preventDefault();
          this.browserProxy_.handler.forwardScrollKey(
              e.key === 'Home' ? ScrollKey.kHome : ScrollKey.kEnd);
        }
        return;
    }
    const scrollKey = scrollKeys[e.key];
    if (scrollKey !== undefined && !e.metaKey && !e.altKey) {
      e.preventDefault();
      this.browserProxy_.handler.forwardScrollKey(scrollKey);
    }
  }

  protected onPreviousClick_() {
    this.browserProxy_.handler.findNext(true);
  }

  protected onNextClick_() {
    this.browserProxy_.handler.findNext(false);
  }

  protected onCloseClick_() {
    this.browserProxy_.handler.close();
  }

  protected hasMatches_(): boolean {
    return this.count_ > 0;
  }

  protected isNoMatch_(): boolean {
    return this.count_ === 0 && this.final_ && this.text_ !== '';
  }

  // "3/12"; "No results" once the page has counted none; nothing before a
  // result or for an empty field.
  protected getCountText_(): string {
    if (this.text_ === '' || this.count_ < 0) {
      return '';
    }
    if (this.count_ === 0) {
      return this.final_ ? 'No results' : '';
    }
    return `${this.active_}/${this.count_}`;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-find-bar-app': CompassFindBarAppElement;
  }
}

customElements.define(CompassFindBarAppElement.is, CompassFindBarAppElement);
