// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassClaudeTurnElement} from './turn.js';

export function getHtml(this: CompassClaudeTurnElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isUser_() ? html`
  <div class="turn user">
    ${this.getPageTitle_() ? html`
      <div class="attached" title="${this.getPageUrl_()}">
        <span class="page-glyph" aria-hidden="true"></span>
        <span>${this.getPageTitle_()}</span>
      </div>` : nothing}
    <div class="bubble">${this.getText_()}</div>
  </div>` : html`
  <div class="turn assistant">
    ${this.getBlocks_().map((item, index) => html`${item.type === 'text' ?
      html`<div class="markdown">${this.renderText_(item.text)}</div>` :
      html`${this.getMemory_(item) ? html`
        <div class="memory" ?undone="${!!this.getMemory_(item)!.undone}">
          <span class="memory-glyph" aria-hidden="true"></span>
          <span class="memory-label"
              title="${this.getMemory_(item)!.text}">${this.getMemoryLabel_(item)}</span>
          ${this.getMemory_(item)!.undone ? html`
            <span class="memory-undone">Undone</span>` : html`
            ${this.getMemory_(item)!.undo !== undefined ? html`
              <button class="link" data-id="${item.id}"
                  @click="${this.onUndoMemoryClick_}">Undo</button>` :
              nothing}`}
        </div>` :
      html`${item.type === 'tool_use' ? html`
        <details class="tool" ?running="${this.getTool_(item).running}"
            ?error="${!!this.getTool_(item).isError}">
          <summary>
            <span class="tool-state" aria-hidden="true"></span>
            <span class="tool-label">${this.getToolLabel_(item)}</span>
          </summary>
          <pre class="tool-input">${this.getToolInput_(item)}</pre>
          ${this.getTool_(item).text ? html`
            <pre class="tool-result">${this.getTool_(item).text}</pre>` :
            nothing}
        </details>
        ${this.getTool_(item).image ? html`
          <img class="shot" src="${this.getTool_(item).image}"
              alt="Screenshot Claude took">` : nothing}` :
      html`${this.showThinking_(item, index) ? html`
        <details class="thinking" ?open="${this.isLive_(index)}">
          <summary>${this.isLive_(index) ? 'Thinking…' : 'Thought'}</summary>
          <div class="thinking-text">${item.thinking}</div>
        </details>` : nothing}`}`}`}`)}
    ${this.isPending_() ? html`
      <div class="pending" aria-label="Claude is answering">
        <span></span><span></span><span></span>
      </div>` : nothing}
    ${this.getStopReason_() === 'refusal' ? html`
      <div class="notice">Claude declined to answer this.</div>` : nothing}
    ${this.getStopReason_() === 'max_tokens' ? html`
      <div class="notice">The answer was cut off at its length limit.</div>` :
      nothing}
    ${this.getError_() ? html`
      <div class="notice error">${this.getError_()}
        <button class="link" @click="${this.onRetryClick_}">Retry</button>
      </div>` : nothing}
  </div>`}
<!--_html_template_end_-->`;
  // clang-format on
}
