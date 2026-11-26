// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {ProfileOption} from '../sidebar.mojom-webui.js';
import type {SpaceSymbol} from '../space_icons.js';
import type {ArchiveDelay} from '../space_options.js';

import type {CompassSpaceEditorAppElement} from './app.js';

export function getHtml(this: CompassSpaceEditorAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.space_ ? html`${this.view_ === 'theme' ? html`
<header>
  <button id="back" class="back" title="Back" aria-label="Back"
      @click="${this.onBackClick_}">
    <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M7.5 2L3.5 6l4 4"></path></svg>
  </button>
  <h1>Theme</h1>
</header>
<main>
  <div class="card padded">
    <compass-theme-editor .space="${this.space_}" .handler="${this.proxy_.handler}">
    </compass-theme-editor>
  </div>
</main>` : html`${this.view_ === 'emoji' ? html`
<header>
  <button id="back" class="back" title="Back" aria-label="Back"
      @click="${this.onBackClick_}">
    <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M7.5 2L3.5 6l4 4"></path></svg>
  </button>
  <h1>Emoji</h1>
</header>
<main>
  <div class="card padded">
    <compass-emoji-picker .selected="${this.getEmoji_()}"
        @emoji-pick="${this.onEmojiPick_}">
    </compass-emoji-picker>
  </div>
</main>` : html`
<header>
  <input id="name" aria-label="Space name" spellcheck="false"
      autocomplete="off" .value="${this.space_.name}"
      ?disabled="${this.space_.viewOnly}"
      @keydown="${this.onNameKeydown_}" @change="${this.onNameChange_}">
  <button id="close" title="Close" aria-label="Close"
      @click="${this.onCloseClick_}">
    <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M2 2l8 8M10 2l-8 8"></path></svg>
  </button>
</header>
<main>
  <div class="card">
    <button id="theme" class="row link" ?disabled="${this.space_.viewOnly}"
        @click="${this.onThemeClick_}">
      <span class="label">Theme</span>
      <span id="themeChip" style="${this.getThemeChipStyle_()}"></span>
      <svg class="chevron" viewBox="0 0 12 12" aria-hidden="true"><path d="M4.5 2l4 4-4 4"></path></svg>
    </button>
  </div>

  <div class="caption">Icon</div>
  <div class="card padded">
    <div id="icons" role="radiogroup" aria-label="Icon">
      <button class="icon-choice" role="radio" data-symbol=""
          title="No icon" aria-label="No icon"
          aria-checked="${this.isIcon_('')}" ?selected="${this.isIcon_('')}"
          ?disabled="${this.space_.viewOnly}" @click="${this.onSymbolClick_}">
        ${this.getInitial_()}
      </button>
      ${this.spaceSymbols_.map((symbol: SpaceSymbol) => html`
        <button class="icon-choice" role="radio" data-symbol="${symbol.name}"
            title="${symbol.label}" aria-label="${symbol.label}"
            aria-checked="${this.isIcon_(symbol.name)}"
            ?selected="${this.isIcon_(symbol.name)}"
            ?disabled="${this.space_!.viewOnly}" @click="${this.onSymbolClick_}">
          <svg viewBox="${this.getSymbolViewBox_()}" aria-hidden="true">
            <path d="${symbol.path}"></path>
          </svg>
        </button>`)}
      <button id="emojiButton" class="icon-choice" title="Emoji"
          aria-label="Emoji" aria-haspopup="dialog"
          ?selected="${!!this.getEmoji_()}" ?disabled="${this.space_.viewOnly}"
          @click="${this.onEmojiClick_}">
        ${this.getEmoji_() || html`
          <svg class="emoji-add" viewBox="0 0 16 16" aria-hidden="true">
            <circle cx="8" cy="8" r="6.25"></circle>
            <path d="M5.6 9.6c.6.9 1.4 1.3 2.4 1.3s1.8-.4 2.4-1.3"></path>
            <circle class="eye" cx="6" cy="6.6" r=".9"></circle>
            <circle class="eye" cx="10" cy="6.6" r=".9"></circle>
          </svg>`}
      </button>
    </div>
  </div>

  <div class="caption">Space</div>
  <div class="card">
    <label id="archiveDelay" class="row">
      <span class="label">Archive Today tabs after</span>
      <select @change="${this.onArchiveDelayChange_}">
        ${this.archiveDelays_.map((delay: ArchiveDelay) => html`
          <option value="${delay.hours}"
              ?selected="${delay.hours === this.space_!.archiveAfterHours}">
            ${delay.label}
          </option>`)}
      </select>
    </label>
    ${this.hasProfiles_() ? html`
      <label id="profile" class="row">
        <span class="label">Profile</span>
        <select @change="${this.onProfileChange_}">
          ${this.profiles_.map((profile: ProfileOption) => html`
            <option value="${profile.id}"
                ?selected="${profile.id === this.space_!.profileId}">
              ${profile.name}
            </option>`)}
          <option value="">New profile…</option>
        </select>
      </label>` : nothing}
    ${this.canChangeSync_() ? html`
      <div class="row">
        <span class="label">Sync to your other devices</span>
        <button id="sync" class="switch" role="switch"
            aria-label="Sync to your other devices"
            aria-checked="${this.space_.synced}" ?checked="${this.space_.synced}"
            @click="${this.onSyncClick_}"></button>
      </div>
      ${this.confirming_ === 'unsync' ? html`
        <div class="confirm">
          <p>Stop syncing “${this.space_.name}”? It’s removed from your other
            devices and stays only on this one.</p>
          <div class="buttons">
            <button class="pill" @click="${this.onConfirmCancelClick_}">Cancel</button>
            <button id="confirmUnsync" class="pill danger-fill"
                @click="${this.onUnsyncConfirmClick_}">Stop syncing</button>
          </div>
        </div>` : nothing}` : nothing}
  </div>

  <div class="actions">
    ${this.canShare_() ? html`
      <button id="share" class="pill" @click="${this.onShareClick_}">
        ${this.getShareLabel_()}
      </button>` : nothing}
    <span class="spacer"></span>
    <button id="delete" class="pill danger" ?disabled="${!this.canDelete_}"
        title="${this.canDelete_ ? '' : 'The last space can’t be deleted'}"
        @click="${this.onDeleteClick_}">Delete space</button>
  </div>
  ${this.confirming_ === 'delete' ? html`
    <div class="confirm">
      <p>Delete “${this.space_.name}”? Its tabs close, and its pinned tabs and
        folders are deleted with it.</p>
      <div class="buttons">
        <button class="pill" @click="${this.onConfirmCancelClick_}">Cancel</button>
        <button id="confirmDelete" class="pill danger-fill"
            @click="${this.onDeleteConfirmClick_}">Delete</button>
      </div>
    </div>` : nothing}
</main>`}`}` : ''}
<!--_html_template_end_-->`;
  // clang-format on
}
