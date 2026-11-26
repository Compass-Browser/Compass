// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {Space} from '../sidebar.mojom-webui.js';
import type {SpaceSymbol} from '../space_icons.js';
import type {ArchiveDelay} from '../space_options.js';

import type {CompassSettingsSpacesPaneElement, SpaceGroup} from './spaces_pane.js';

export function getHtml(this: CompassSettingsSpacesPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${repeat(this.getGroups_(), (group: SpaceGroup) => group.profileId,
    (group: SpaceGroup, index: number) => html`
  <section class="row-group" data-row="${index === 0 ? 'spaces' : nothing}">
    <div class="group-header">
      <h2 class="group-title">${this.getGroupTitle_(group)}</h2>
      <button class="pill small" data-profile-id="${group.profileId}"
          aria-label="New space in ${group.profileName}"
          @click="${this.onAddClick_}">
        New space
      </button>
    </div>
    <div class="group">
      ${group.spaces.length === 0 ? html`
        <div class="row control-row">
          <span class="row-text">
            <span class="row-detail">
              No spaces in the switcher. A new space brings the profile in.
            </span>
          </span>
        </div>` : nothing}
      ${repeat(group.spaces, (space: Space) => space.id,
          (space: Space) => html`
        <div class="row control-row space-row" data-space-id="${space.id}"
            ?expanded="${this.isExpanded_(space)}">
          <span class="space-swatch" style="${this.getSpaceStyle_(space)}"
              aria-hidden="true">
            ${this.getSpaceSymbol_(space) ? html`
              <svg viewBox="${this.getSymbolViewBox_()}">
                <path d="${this.getSpaceSymbol_(space)!.path}"></path>
              </svg>` : html`<span>${this.getSpaceIconText_(space)}</span>`}
          </span>
          <span class="row-text">
            <input class="field space-name" type="text"
                aria-label="Name of ${space.name}" data-space-id="${space.id}"
                spellcheck="false" autocomplete="off"
                ?disabled="${space.viewOnly}" .value="${space.name}"
                @change="${this.onNameChange_}"
                @keydown="${this.onNameKeydown_}">
            <span class="row-detail" ?hidden="${!this.getSpaceDetail_(space)}">
              ${this.getSpaceDetail_(space)}
            </span>
          </span>
          <button class="pill small move" data-space-id="${space.id}"
              data-step="up" title="Move up" aria-label="Move ${space.name} up"
              ?disabled="${!this.canMoveUp_(space)}"
              @click="${this.onMoveClick_}">
            ↑
          </button>
          <button class="pill small move" data-space-id="${space.id}"
              data-step="down" title="Move down"
              aria-label="Move ${space.name} down"
              ?disabled="${!this.canMoveDown_(space)}"
              @click="${this.onMoveClick_}">
            ↓
          </button>
          <button class="pill small" data-space-id="${space.id}"
              aria-expanded="${this.isExpanded_(space)}"
              @click="${this.onExpandClick_}">
            ${this.isExpanded_(space) ? 'Done' : 'Edit…'}
          </button>
        </div>
        ${this.isExpanded_(space) ? html`
          <div class="space-details">
            <div class="row control-row space-detail-row">
              <span class="row-text"><span class="row-label">Icon</span></span>
              <div class="space-icons" role="radiogroup" aria-label="Icon">
                <button class="icon-choice" role="radio"
                    data-space-id="${space.id}" data-symbol=""
                    title="No icon" aria-label="No icon"
                    aria-checked="${this.isIcon_(space, '')}"
                    ?selected="${this.isIcon_(space, '')}"
                    ?disabled="${space.viewOnly}"
                    @click="${this.onSymbolClick_}">
                  ${this.getSpaceInitial_(space)}
                </button>
                ${this.spaceSymbols_.map((symbol: SpaceSymbol) => html`
                  <button class="icon-choice" role="radio"
                      data-space-id="${space.id}" data-symbol="${symbol.name}"
                      title="${symbol.label}" aria-label="${symbol.label}"
                      aria-checked="${this.isIcon_(space, symbol.name)}"
                      ?selected="${this.isIcon_(space, symbol.name)}"
                      ?disabled="${space.viewOnly}"
                      @click="${this.onSymbolClick_}">
                    <svg viewBox="${this.getSymbolViewBox_()}"
                        aria-hidden="true">
                      <path d="${symbol.path}"></path>
                    </svg>
                  </button>`)}
                <button class="icon-choice emoji-choice"
                    title="Emoji" aria-label="Emoji"
                    aria-expanded="${this.isEmojiOpen_(space)}"
                    ?selected="${!!space.icon?.emoji}"
                    ?disabled="${space.viewOnly}"
                    @click="${this.onEmojiButtonClick_}">
                  ${space.icon?.emoji ?? html`
                    <svg class="emoji-add" viewBox="0 0 16 16"
                        aria-hidden="true">
                      <circle cx="8" cy="8" r="6.25"></circle>
                      <path d="M5.6 9.6c.6.9 1.4 1.3 2.4 1.3s1.8-.4 2.4-1.3">
                      </path>
                      <circle class="eye" cx="6" cy="6.6" r=".9"></circle>
                      <circle class="eye" cx="10" cy="6.6" r=".9"></circle>
                    </svg>`}
                </button>
              </div>
            </div>
            ${this.isEmojiOpen_(space) ? html`
              <div class="row space-emoji-row">
                <compass-emoji-picker data-space-id="${space.id}"
                    .selected="${space.icon?.emoji ?? ''}"
                    @emoji-pick="${this.onEmojiPick_}"
                    @keydown="${this.onEmojiKeydown_}">
                </compass-emoji-picker>
              </div>` : nothing}
            <div class="row control-row space-detail-row">
              <span class="row-text"><span class="row-label">Theme</span></span>
              <span class="theme-chip" style="${this.getSpaceStyle_(space)}">
              </span>
              <button class="pill small"
                  aria-expanded="${this.isThemeOpen_(space)}"
                  ?disabled="${space.viewOnly}" @click="${this.onThemeClick_}">
                ${this.isThemeOpen_(space) ? 'Done' : 'Edit theme…'}
              </button>
            </div>
            ${this.isThemeOpen_(space) ? html`
              <div class="row space-theme-row">
                <compass-theme-editor .space="${space}"
                    .handler="${this.getHandler_()}">
                </compass-theme-editor>
              </div>` : nothing}
            <div class="row control-row space-detail-row"
                ?hidden="${!this.hasProfiles_()}">
              <span class="row-text">
                <span class="row-label">Profile</span>
                <span class="row-detail">
                  Its open tabs close; its pinned and Today tabs move with it.
                </span>
              </span>
              <select class="popup" aria-label="Profile of ${space.name}"
                  data-space-id="${space.id}" @change="${this.onProfileChange_}">
                ${this.profiles_.map(profile => html`
                  <option value="${profile.id}"
                      ?selected="${profile.id === space.profileId}">
                    ${profile.name}
                  </option>`)}
                <option value="">New profile…</option>
              </select>
            </div>
            <div class="row control-row space-detail-row">
              <span class="row-text">
                <span class="row-label">Archive Today tabs after</span>
              </span>
              <select class="popup" aria-label="Archive Today tabs after"
                  data-space-id="${space.id}"
                  @change="${this.onArchiveDelayChange_}">
                ${this.archiveDelays_.map((delay: ArchiveDelay) => html`
                  <option value="${delay.hours}"
                      ?selected="${delay.hours === space.archiveAfterHours}">
                    ${delay.label}
                  </option>`)}
              </select>
            </div>
            <label class="row control-row space-detail-row"
                ?hidden="${!this.canChangeSync_(space)}">
              <span class="row-text">
                <span class="row-label">Sync to your other devices</span>
              </span>
              <input type="checkbox" class="switch" data-space-id="${space.id}"
                  .checked="${space.synced}" @change="${this.onSyncChange_}">
            </label>
            <div class="row control-row confirm-row"
                ?hidden="${!this.isConfirmingUnsync_(space)}">
              <span class="row-text">
                <span class="row-detail">
                  “${space.name}” stays on this device and is removed from your
                  other devices.
                </span>
              </span>
              <button class="pill" @click="${this.onUnsyncCancelClick_}">
                Cancel
              </button>
              <button class="pill danger" data-space-id="${space.id}"
                  @click="${this.onUnsyncConfirmClick_}">
                Stop syncing
              </button>
            </div>
            <div class="row control-row space-detail-row"
                ?hidden="${this.isConfirmingDelete_(space)}">
              <span class="row-text"></span>
              <button class="pill export" data-space-id="${space.id}"
                  @click="${this.onExportClick_}">
                Export…
              </button>
              <button class="pill danger" data-space-id="${space.id}"
                  ?disabled="${!this.canDelete_()}"
                  @click="${this.onDeleteClick_}">
                Delete space…
              </button>
            </div>
            <div class="row control-row confirm-row"
                ?hidden="${!this.isConfirmingDelete_(space)}">
              <span class="row-text">
                <span class="row-detail">${this.getDeleteText_(space)}</span>
              </span>
              <button class="pill" @click="${this.onDeleteCancelClick_}">
                Cancel
              </button>
              <button class="pill danger" data-space-id="${space.id}"
                  @click="${this.onDeleteConfirmClick_}">
                Delete
              </button>
            </div>
          </div>` : nothing}`)}
    </div>
  </section>`)}
<section class="row-group">
  <div class="group">
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Export spaces</span>
        <span class="row-detail">
          Every space's pinned and Today tabs, and the favorites, as a
          bookmarks file other browsers can import.
        </span>
      </span>
      <button class="pill" id="exportAll" @click="${this.onExportAllClick_}">
        Export…
      </button>
    </div>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
