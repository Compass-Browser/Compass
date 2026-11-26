// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassEmojiPickerElement, Emoji, EmojiGroup} from './emoji_picker.js';
import {i18n} from './i18n.js';

export function getHtml(this: CompassEmojiPickerElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<input id="search" type="search" aria-label="${i18n('emojiPickerSearch')}"
    placeholder="${i18n('emojiPickerSearch')}" spellcheck="false" autocomplete="off"
    @input="${this.onSearchInput_}" @keydown="${this.onSearchKeydown_}">
<div id="groups" role="tablist" aria-label="${i18n('emojiPickerCategories')}">
  ${this.groups_.map((group: EmojiGroup) => html`
    <button class="group" role="tab" data-group="${group.id}"
        title="${group.label}" aria-label="${group.label}"
        aria-selected="${this.isGroup_(group)}"
        ?selected="${this.isGroup_(group)}"
        @click="${this.onGroupClick_}">
      ${group.icon}
    </button>`)}
</div>
<div id="grid" role="listbox" aria-label="${i18n('spaceEditorEmoji')}"
    @keydown="${this.onGridKeydown_}">
  ${this.shown_.map((emoji: Emoji) => html`
    <button class="emoji" role="option" data-emoji="${emoji.emoji}"
        title="${emoji.name}" aria-label="${emoji.name}"
        aria-selected="${this.isSelected_(emoji)}"
        ?selected="${this.isSelected_(emoji)}"
        @click="${this.onEmojiClick_}">
      ${emoji.emoji}
    </button>`)}
  ${!this.isLoading_() && this.shown_.length === 0 ? html`
    <div id="empty">${i18n('emojiPickerNoResults')}</div>` : nothing}
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
