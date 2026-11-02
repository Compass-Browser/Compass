// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {ContentBlock, Turn} from './conversation.js';
import {renderMarkdown} from './markdown.js';
import {getCss} from './turn.css.js';
import {getHtml} from './turn.html.js';

// One message of the chat: the user's, with the page it carried, or
// Claude's, rendered from its content blocks as they stream in. The turn is
// changed in place; |revision| tells the element to render it again. Fires
// "retry" from a failed reply's Retry button.
export class CompassClaudeTurnElement extends CrLitElement {
  static get is() {
    return 'compass-claude-turn';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      turn: {type: Object},
      revision: {type: Number},
    };
  }

  accessor turn: Turn|null = null;
  accessor revision: number = 0;

  protected isUser_(): boolean {
    return this.turn?.role === 'user';
  }

  protected getText_(): string {
    return this.turn?.role === 'user' ? this.turn.text : '';
  }

  protected getPageTitle_(): string {
    return this.turn?.role === 'user' ? this.turn.pageTitle || '' : '';
  }

  protected getPageUrl_(): string {
    return this.turn?.role === 'user' ? this.turn.pageUrl || '' : '';
  }

  protected getBlocks_(): ContentBlock[] {
    return this.turn?.role === 'assistant' ? this.turn.content : [];
  }

  // The last block of a reply still streaming.
  protected isLive_(index: number): boolean {
    return this.turn?.role === 'assistant' && this.turn.streaming &&
        index === this.turn.content.length - 1;
  }

  protected showThinking_(item: ContentBlock, index: number): boolean {
    return item.type === 'thinking' && (!!item.thinking || this.isLive_(index));
  }

  protected isPending_(): boolean {
    return this.turn?.role === 'assistant' && this.turn.streaming &&
        !this.turn.content.length;
  }

  protected getStopReason_(): string {
    return this.turn?.role === 'assistant' ? this.turn.stopReason || '' : '';
  }

  protected getError_(): string {
    return this.turn?.role === 'assistant' ? this.turn.error || '' : '';
  }

  protected renderText_(text: string|undefined): DocumentFragment {
    return renderMarkdown(text || '');
  }

  protected onRetryClick_() {
    this.fire('retry');
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-claude-turn': CompassClaudeTurnElement;
  }
}

customElements.define(CompassClaudeTurnElement.is, CompassClaudeTurnElement);
