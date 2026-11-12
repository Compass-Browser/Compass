// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {Result} from '../command_bar.mojom-webui.js';
import {Mode, ResultKind} from '../command_bar.mojom-webui.js';
import type {SpaceSymbol} from '../space_icons.js';
import {findSpaceSymbol, SPACE_SYMBOL_VIEW_BOX} from '../space_icons.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {CommandBarBrowserProxy} from './browser_proxy.js';

// The command bar: a field, and the browser's results for what's typed in
// it. Enter acts on the selected result, Cmd+Enter (Ctrl+Enter outside
// macOS) opens a suggestion as a new entry, Esc closes. The page stays loaded
// between openings; Open() starts it over. See docs/design/command-bar.md.
export class CompassCommandBarAppElement extends CrLitElement {
  static get is() {
    return 'compass-command-bar-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      mode_: {type: Number},
      results_: {type: Array},
      selectedIndex_: {type: Number},
    };
  }

  protected accessor mode_: Mode = Mode.kNewEntry;
  protected accessor results_: Result[] = [];
  protected accessor selectedIndex_: number = 0;

  private browserProxy_: CommandBarBrowserProxy =
      CommandBarBrowserProxy.getInstance();
  private listenerIds_: number[] = [];

  // What the user typed, without the inline autocompletion.
  private typed_: string = '';
  // Set by Open(): the browser shows the bubble once the first results for
  // the new text are rendered, so it opens at its final size.
  private showPending_: boolean = false;
  // The pointer's screen position at its last event since Open(), or null.
  // Rows that change under a resting pointer get a pointermove without
  // movement, which mustn't select them (docs/design/command-bar.md,
  // "Pointer").
  private pointerPosition_: {x: number, y: number}|null = null;

  override connectedCallback() {
    super.connectedCallback();
    // Follows the window's theme, which follows its space.
    ColorChangeUpdater.forDocument().start();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.open.addListener(this.onOpen_.bind(this)),
      router.setResults.addListener(this.onSetResults_.bind(this)),
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

  private onOpen_(mode: Mode, text: string) {
    this.mode_ = mode;
    this.typed_ = text;
    this.results_ = [];
    this.selectedIndex_ = 0;
    this.showPending_ = true;
    this.pointerPosition_ = null;
    const input = this.input_;
    input.value = text;
    input.select();
    input.focus();
    // Starting from the page's URL, don't complete it into another.
    this.browserProxy_.handler.query(text, text !== '');
  }

  private async onSetResults_(
      text: string, results: Result[], inlineAutocompletion: string) {
    if (text !== this.typed_) {
      return;
    }
    this.results_ = results;
    this.selectedIndex_ = 0;

    // Complete the text in the field, selected, while the caret is at its end.
    const input = this.input_;
    if (inlineAutocompletion && input.value === this.typed_ &&
        input.selectionStart === this.typed_.length &&
        input.selectionEnd === this.typed_.length) {
      input.value = this.typed_ + inlineAutocompletion;
      input.setSelectionRange(this.typed_.length, input.value.length);
    }

    if (this.showPending_) {
      this.showPending_ = false;
      await this.updateComplete;
      this.browserProxy_.handler.showUI();
      this.input_.focus();
    }
  }

  protected onInput_(e: InputEvent) {
    this.typed_ = this.input_.value;
    // After a deletion, completing would put back what was just deleted.
    this.browserProxy_.handler.query(
        this.typed_, e.inputType.startsWith('delete'));
  }

  protected onKeydown_(e: KeyboardEvent) {
    switch (e.key) {
      case 'ArrowDown':
      case 'ArrowUp': {
        e.preventDefault();
        const count = this.results_.length;
        if (count) {
          const step = e.key === 'ArrowDown' ? 1 : -1;
          this.selectedIndex_ = (this.selectedIndex_ + step + count) % count;
          this.scrollSelectedIntoView_();
        }
        break;
      }
      case 'Tab': {
        // Focus stays in the field; Tab takes the completion to type on.
        e.preventDefault();
        if (!e.shiftKey && !e.altKey && !e.ctrlKey && !e.metaKey) {
          this.takeCompletion_();
        }
        break;
      }
      case 'Enter': {
        e.preventDefault();
        const result = this.results_[this.selectedIndex_];
        const newEntry = navigator.platform.startsWith('Mac') ? e.metaKey :
                                                                e.ctrlKey;
        this.accept_(result ? result.id : 0, newEntry);
        break;
      }
    }
  }

  protected onResultPointermove_(e: PointerEvent) {
    const last = this.pointerPosition_;
    this.pointerPosition_ = {x: e.screenX, y: e.screenY};
    if (!last || (last.x === e.screenX && last.y === e.screenY) ||
        (e.movementX === 0 && e.movementY === 0)) {
      return;
    }
    const index = this.getResultIndex_(e);
    if (index !== -1) {
      this.selectedIndex_ = index;
    }
  }

  protected onResultClick_(e: MouseEvent) {
    const index = this.getResultIndex_(e);
    if (index !== -1) {
      this.accept_(this.results_[index]!.id, e.metaKey || e.ctrlKey);
    }
  }

  // Tab: the field's inline completion becomes typed text, caret at its end;
  // without one, a selected search suggestion fills the field. Then queries
  // the new text, so more words can follow.
  private takeCompletion_() {
    const input = this.input_;
    let text = '';
    if (input.value.length > this.typed_.length &&
        input.value.startsWith(this.typed_) &&
        input.selectionStart === this.typed_.length &&
        input.selectionEnd === input.value.length) {
      text = input.value;
    } else {
      const result = this.results_[this.selectedIndex_];
      if (result?.kind === ResultKind.kSearch &&
          result.title !== this.typed_) {
        text = result.title;
      }
    }
    if (!text) {
      return;
    }
    input.value = text;
    input.setSelectionRange(text.length, text.length);
    this.typed_ = text;
    this.browserProxy_.handler.query(text, false);
  }

  private accept_(id: number, newEntry: boolean) {
    this.browserProxy_.handler.accept(id, this.typed_, newEntry);
  }

  private getResultIndex_(e: Event): number {
    const row = (e.target as HTMLElement).closest<HTMLElement>('.result');
    return row ? Number(row.dataset['index']) : -1;
  }

  private async scrollSelectedIntoView_() {
    await this.updateComplete;
    this.shadowRoot.querySelector('.result[selected]')
        ?.scrollIntoView({block: 'nearest'});
  }

  protected getPlaceholder_(): string {
    return this.mode_ === Mode.kNewEntry ? 'Search or enter URL…' :
                                           'Search, enter URL or a command…';
  }

  protected isSelected_(index: number): boolean {
    return index === this.selectedIndex_;
  }

  protected hasFavicon_(result: Result): boolean {
    return result.iconUrl !== '';
  }

  protected getFaviconUrl_(result: Result): string {
    const params = new URLSearchParams({
      size: '16',
      scaleFactor: `${window.devicePixelRatio}x`,
      pageUrl: result.iconUrl,
      allowGoogleServerFallback: '0',
    });
    return `chrome://favicon2/?${params.toString()}`;
  }

  // A space's symbol (docs/design/spaces.md, "Space icons"); an unknown one
  // (from a later version) shows the glyph.
  protected getSpaceSymbol_(result: Result): SpaceSymbol|null {
    return result.spaceSymbol ? findSpaceSymbol(result.spaceSymbol) : null;
  }

  protected getSymbolViewBox_(): string {
    return SPACE_SYMBOL_VIEW_BOX;
  }

  // The glyph rows without a favicon or a space icon show.
  protected getGlyph_(result: Result): string {
    switch (result.kind) {
      case ResultKind.kSearch:
        return '⌕';
      case ResultKind.kSpace:
        return '◐';
      case ResultKind.kCommand:
        return '›';
      default:
        return '◦';
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-command-bar-app': CompassCommandBarAppElement;
  }
}

customElements.define(
    CompassCommandBarAppElement.is, CompassCommandBarAppElement);
