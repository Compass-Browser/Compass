// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF, i18nPlural} from '../i18n.js';

import type {CompassClaudeChatAppElement} from './app.js';

export function getHtml(this: CompassClaudeChatAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<header>
  <span class="logo" aria-hidden="true"></span>
  <span class="title">Claude</span>
  <select class="model" id="model" title="${i18n('claudeChatModel')}"
      aria-label="${i18n('claudeChatModel')}"
      ?disabled="${this.streaming_}" @change="${this.onModelChange_}">
    ${this.getModels_().map(m => html`
      <option value="${m.id}" .selected="${m.id === this.model_}">
        ${m.name}
      </option>`)}
  </select>
  <span class="spacer"></span>
  <button class="icon" id="newChat" title="${i18n('claudeChatNewChat')}"
      aria-label="${i18n('claudeChatNewChat')}"
      ?disabled="${!this.hasTurns_()}" @click="${this.onNewChatClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M8 3.5v9M3.5 8h9"></path>
    </svg>
  </button>
  <button class="icon" id="close" title="${i18n('close')}"
      aria-label="${i18n('close')}"
      @click="${this.onCloseClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M4.5 4.5l7 7M11.5 4.5l-7 7"></path>
    </svg>
  </button>
</header>

<main id="messages" @click="${this.onMessagesClick_}">
  ${!this.hasKey_ ? html`
    <div class="empty">
      <p class="lead">${i18n('claudeChatNoKeyLead')}</p>
      <p>${i18n('claudeChatNoKey')}</p>
      <button id="openSettings" @click="${this.onOpenSettingsClick_}">
        ${i18n('claudeChatAddKey')}
      </button>
    </div>` : html`${!this.hasTurns_() ? html`
    <div class="empty">
      <p class="lead">${i18n('claudeChatEmptyLead')}</p>
      ${this.pageReadable_ ? html`
        <div class="suggestions">
          ${this.suggestions_.map(text => html`
            <button class="suggestion" @click="${this.onSuggestionClick_}"
                data-text="${text}">${text}</button>`)}
        </div>` : nothing}
    </div>` : html`${this.getShownTurns_().map(turn => html`
      <compass-claude-turn .turn="${turn}" .tools="${this.conversation.tools}"
          .revision="${this.revision_}"
          @retry="${this.onTurnRetry_}"
          @undo-memory="${this.onTurnUndoMemory_}"></compass-claude-turn>`)}`}`}
</main>

<footer>
  ${this.streaming_ && this.taskTitle_ ? html`
    <div id="using" class="chip using" title="${i18n('claudeChatUsingTooltip')}">
      <span class="pulse" aria-hidden="true"></span>
      <span class="chip-text">${i18nF('claudeChatUsing', this.taskTitle_)}</span>
    </div>` : html`${this.pageReadable_ ? html`
    <button id="attach" class="chip" ?on="${this.attachPage_}"
        aria-pressed="${this.attachPage_}" title="${this.getAttachTitle_()}"
        @click="${this.onAttachClick_}">
      <span class="page-glyph" aria-hidden="true"></span>
      <span class="chip-text">${this.pageTitle_ || this.pageUrl_}</span>
    </button>` : nothing}`}
  ${this.approvals_.length ? html`
    <div class="approval" id="approval" role="alertdialog"
        aria-labelledby="approvalTitle" aria-describedby="approvalDetail">
      <div class="approval-title" id="approvalTitle">
        ${this.approvals_[0]!.title}
      </div>
      ${this.approvals_[0]!.detail ? html`
        <pre class="approval-detail" id="approvalDetail">${
            this.approvals_[0]!.detail}</pre>` : nothing}
      <div class="approval-buttons">
        ${this.approvals_.length > 1 ? html`
          <span class="approval-more">
            ${i18nPlural('claudeChatMoreWaiting', this.approvals_.length - 1)}
          </span>` : nothing}
        <span class="spacer"></span>
        <button class="approval-button" id="deny"
            @click="${this.onDenyClick_}">${i18n('claudeChatDeny')}</button>
        <button class="approval-button" id="allowForChat"
            @click="${this.onAllowForChatClick_}">
          ${i18n('claudeChatAllowForChat')}
        </button>
        <button class="approval-button primary" id="allow"
            @click="${this.onAllowClick_}">${i18n('claudeChatAllow')}</button>
      </div>
    </div>` : nothing}
  ${this.queued_.length ? html`
    <div class="queued" id="queued" aria-label="${i18n('claudeChatQueued')}">
      <div class="queued-label">${this.getQueuedLabel_()}</div>
      ${this.queued_.map((item, index) => html`
        <div class="queued-item">
          <button class="queued-text" data-index="${index}"
              title="${i18n('edit')}" @click="${this.onQueuedEditClick_}">
            ${item.text}
          </button>
          <button class="icon queued-remove" data-index="${index}"
              title="${i18n('remove')}" aria-label="${i18n('remove')}"
              @click="${this.onQueuedRemoveClick_}">
            <svg viewBox="0 0 16 16" aria-hidden="true">
              <path d="M5 5l6 6M11 5l-6 6"></path>
            </svg>
          </button>
        </div>`)}
    </div>` : nothing}
  <div class="composer">
    <textarea id="input" rows="1" placeholder="${this.getPlaceholder_()}"
        aria-label="${i18n('claudeChatMessage')}" ?disabled="${!this.hasKey_}"
        @input="${this.onInputInput_}"
        @keydown="${this.onInputKeydown_}"></textarea>
    ${this.canQueue_() ? html`
      <button class="send" id="queue" title="${i18n('claudeChatQueueTooltip')}"
          aria-label="${i18n('claudeChatQueue')}" @click="${this.onQueueClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <path d="M8 12.5v-9M4 7.5l4-4 4 4"></path>
        </svg>
      </button>` : nothing}
    ${this.streaming_ ? html`
      <button class="send" id="stop" title="${i18n('claudeChatStopTooltip')}"
          aria-label="${i18n('claudeChatStop')}"
          @click="${this.onStopClick_}">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <rect x="4.5" y="4.5" width="7" height="7" rx="1"></rect>
        </svg>
      </button>` : html`
      <button class="send" id="send" title="${i18n('claudeChatSendTooltip')}"
          aria-label="${i18n('claudeChatSend')}"
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
