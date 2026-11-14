// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {Result} from '../command_bar.mojom-webui.js';
import {Mode, ResultKind} from '../command_bar.mojom-webui.js';
import type {SpaceSymbol} from '../space_icons.js';
import {findSpaceSymbol, SPACE_SYMBOL_VIEW_BOX} from '../space_icons.js';

import type {CommandSymbol} from './command_icons.js';
import {COMMAND_SYMBOL_VIEW_BOX, findCommandSymbol} from './command_icons.js';

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
      aboutPage_: {type: Boolean},
      answer_: {type: String},
      answerDone_: {type: Boolean},
      answerError_: {type: String},
      asked_: {type: String},
      mode_: {type: Number},
      results_: {type: Array},
      selectedIndex_: {type: Number},
    };
  }

  // Ask in the command bar (docs/design/ai-sidebar.md): "About this page"
  // for the next question, the question asked (empty while there's none, and
  // the results show), and its answer so far.
  protected accessor aboutPage_: boolean = true;
  protected accessor answer_: string = '';
  protected accessor answerDone_: boolean = false;
  protected accessor answerError_: string = '';
  protected accessor asked_: string = '';
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
      router.onAnswer.addListener((text: string) => {
        if (this.asked_) {
          this.answer_ = text;
        }
      }),
      router.onAnswerDone.addListener((error: string) => {
        if (this.asked_) {
          this.answerDone_ = true;
          this.answerError_ = error;
        }
      }),
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
    this.clearAnswer_();
    this.aboutPage_ = true;
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
    // A new line: the results again.
    this.clearAnswer_();
    this.typed_ = this.input_.value;
    // After a deletion, completing would put back what was just deleted.
    this.browserProxy_.handler.query(
        this.typed_, e.inputType.startsWith('delete'));
  }

  protected onKeydown_(e: KeyboardEvent) {
    if (this.asked_) {
      // The answer shows: Enter (or Tab) continues in the chat; Esc closes
      // the bar, as always.
      if (e.key === 'Enter' || e.key === 'Tab') {
        e.preventDefault();
        if (e.key === 'Enter' && !e.isComposing) {
          this.continueInChat_();
        }
      } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
      }
      return;
    }
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
        // Focus stays in the field; Tab takes the completion to type on, or
        // asks on the "Ask Claude" row.
        e.preventDefault();
        if (!e.shiftKey && !e.altKey && !e.ctrlKey && !e.metaKey) {
          if (this.results_[this.selectedIndex_]?.kind ===
              ResultKind.kAskClaude) {
            this.ask_(this.results_[this.selectedIndex_]!);
          } else {
            this.takeCompletion_();
          }
        }
        break;
      }
      case 'Enter': {
        e.preventDefault();
        const result = this.results_[this.selectedIndex_];
        if (result?.kind === ResultKind.kAskClaude) {
          this.ask_(result);
          break;
        }
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
    if ((e.target as HTMLElement).closest('.about')) {
      // "About this page" on the Ask row: a switch, not the row.
      this.aboutPage_ = !this.aboutPage_;
      this.input_.focus();
      return;
    }
    const index = this.getResultIndex_(e);
    if (index === -1) {
      return;
    }
    const result = this.results_[index]!;
    if (result.kind === ResultKind.kAskClaude) {
      this.ask_(result);
      return;
    }
    this.accept_(result.id, e.metaKey || e.ctrlKey);
  }

  // Asks Claude the row's question; the answer streams in under the field.
  private ask_(result: Result) {
    this.clearAnswer_();
    this.asked_ = result.subtitle;
    this.browserProxy_.handler.ask(
        result.subtitle, this.aboutPage_ && result.aboutPage !== '');
    this.input_.focus();
  }

  private continueInChat_() {
    if (this.asked_) {
      this.browserProxy_.handler.continueInChat();
    }
  }

  protected onContinueClick_() {
    this.continueInChat_();
  }

  protected getAnswerText_(): string {
    if (this.answer_) {
      return this.answer_;
    }
    return this.answerDone_ ? '' : 'Claude is answering…';
  }

  private clearAnswer_() {
    this.asked_ = '';
    this.answer_ = '';
    this.answerDone_ = false;
    this.answerError_ = '';
  }

  protected isAsking_(): boolean {
    return this.asked_ !== '';
  }

  // The "About this page" switch shows on the Ask row when there's a page.
  protected hasAboutPage_(result: Result): boolean {
    return result.kind === ResultKind.kAskClaude && result.aboutPage !== '';
  }

  protected getAboutTitle_(result: Result): string {
    return this.aboutPage_ ?
        `Claude gets this tab's title and site: ${result.aboutPage}` :
        'Only the question goes to Claude';
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

  // A command's symbol (command_icons.ts); null for other rows and for one
  // the page doesn't know, which show the glyph.
  protected getCommandSymbol_(result: Result): CommandSymbol|null {
    return result.commandSymbol ? findCommandSymbol(result.commandSymbol) :
                                  null;
  }

  protected getCommandViewBox_(symbol: CommandSymbol): string {
    return symbol.viewBox || COMMAND_SYMBOL_VIEW_BOX;
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
      case ResultKind.kAskClaude:
        return '✦';
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
