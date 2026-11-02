// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassClaudeChatAppElement} from './app.js';

export function getHtml(this: CompassClaudeChatAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<header>
  <span class="logo" aria-hidden="true"></span>
  <span class="title">Claude</span>
  <span class="model" title="Model (Claude settings)">${this.model_}</span>
  <span class="spacer"></span>
  <button class="icon" id="newChat" title="New chat" aria-label="New chat"
      ?disabled="${!this.hasTurns_()}" @click="${this.onNewChatClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M8 3.5v9M3.5 8h9"></path>
    </svg>
  </button>
  <button class="icon" id="close" title="Close" aria-label="Close"
      @click="${this.onCloseClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M4.5 4.5l7 7M11.5 4.5l-7 7"></path>
    </svg>
  </button>
</header>

<main id="messages" @click="${this.onMessagesClick_}">
  ${!this.hasKey_ ? html`
    <div class="empty">
      <p class="lead">Chat with Claude about the page you're on.</p>
      <p>Compass ships no key: add your own Anthropic API key in settings.</p>
      <button id="openSettings" @click="${this.onOpenSettingsClick_}">
        Add an API key
      </button>
    </div>` : html`${!this.hasTurns_() ? html`
    <div class="empty">
      <p class="lead">Ask about the page you're on, or anything else.</p>
      ${this.pageReadable_ ? html`
        <div class="suggestions">
          ${this.suggestions_.map(text => html`
            <button class="suggestion" @click="${this.onSuggestionClick_}"
                data-text="${text}">${text}</button>`)}
        </div>` : nothing}
    </div>` : html`${this.getShownTurns_().map(turn => html`
      <compass-claude-turn .turn="${turn}" .tools="${this.conversation.tools}"
          .revision="${this.revision_}"
          @retry="${this.onTurnRetry_}"></compass-claude-turn>`)}`}`}
</main>

<footer>
  ${this.streaming_ && this.taskTitle_ ? html`
    <div id="using" class="chip using" title="Claude reads and acts in this tab">
      <span class="pulse" aria-hidden="true"></span>
      <span class="chip-text">Using ${this.taskTitle_}</span>
    </div>` : html`${this.pageReadable_ ? html`
    <button id="attach" class="chip" ?on="${this.attachPage_}"
        aria-pressed="${this.attachPage_}" title="${this.getAttachTitle_()}"
        @click="${this.onAttachClick_}">
      <span class="page-glyph" aria-hidden="true"></span>
      <span class="chip-text">${this.pageTitle_ || this.pageUrl_}</span>
    </button>` : nothing}`}
  <div class="composer">
    <textarea id="input" rows="1" placeholder="Ask Claude…"
        aria-label="Message to Claude" ?disabled="${!this.hasKey_}"
        @input="${this.onInputInput_}"
        @keydown="${this.onInputKeydown_}"></textarea>
    ${this.streaming_ ? html`
      <button class="send" id="stop" title="Stop (Esc)" aria-label="Stop"
          @click="${this.onStopClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <rect x="4.5" y="4.5" width="7" height="7" rx="1"></rect>
        </svg>
      </button>` : html`
      <button class="send" id="send" title="Send (Enter)" aria-label="Send"
          ?disabled="${!this.canSend_()}" @click="${this.onSendClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <path d="M8 12.5v-9M4 7.5l4-4 4 4"></path>
        </svg>
      </button>`}
  </div>
</footer>
<!--_html_template_end_-->`;
  // clang-format on
}
