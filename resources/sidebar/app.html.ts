// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {ArchiveDelay, CompassSidebarAppElement, Preview} from './app.js';
import type {SpaceSymbol} from './space_icons.js';
import {speakerIcon} from './speaker_icon.js';
import type {ArchivedEntry, Node, ProfileOption, Space} from './sidebar.mojom-webui.js';

export function getHtml(this: CompassSidebarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="favorites" role="list" aria-label="Favorites"
    ?empty="${this.isFavoritesEmpty_()}" ?dragging="${this.dragActive_}"
    @pointerover="${this.onSectionsPointerover_}"
    @pointerleave="${this.onSectionsPointerleave_}"
    @pointerdown="${this.onSectionsPointerdown_}"
    @focusin="${this.onFavoritesFocusin_}"
    @focusout="${this.onSectionsFocusout_}"
    @keydown="${this.onFavoritesKeydown_}"
    @contextmenu="${this.onEntriesContextmenu_}"
    @dragstart="${this.onDragstart_}"
    @dragover="${this.onDragover_}" @dragleave="${this.onDragleave_}"
    @drop="${this.onDrop_}" @dragend="${this.onDragend_}">
  ${this.isFavoritesEmpty_() ? html`
    <div class="hint" data-flip="favorites-hint">
      Drop here to add to Favorites
    </div>` : nothing}
  ${repeat(this.getFavorites_(), (node: Node) => node.id, (node: Node) => html`
    <div class="tile" role="listitem" draggable="true" tabindex="-1"
        data-node-id="${node.id}" title="${node.customTitle || node.title || node.url}"
        ?active="${this.isActive_(node)}" ?unloaded="${!node.live}"
        ?claude="${node.claude}" @click="${this.onTileClick_}">
      <img class="favicon" alt="" src="${this.getFaviconUrl_(node.url, 32)}">
      ${this.hasAudio_(node) ? html`
        <button class="audio" data-node-id="${node.id}"
            ?muted="${this.isMuted_(node)}"
            aria-label="${this.getAudioLabel_(node)}"
            title="${this.getAudioLabel_(node)}"
            @click="${this.onTileAudioClick_}">
          ${speakerIcon(this.isMuted_(node))}
        </button>` : nothing}
      <button class="close" data-node-id="${node.id}"
          aria-label="${this.getTileCloseLabel_(node)}"
          title="${this.getTileCloseLabel_(node)}"
          @click="${this.onTileCloseClick_}">&times;</button>
    </div>`)}
</div>
<div id="stage">
  <div id="spacePane">
    <div id="spaceName" title="Double-click to edit the space"
        @dblclick="${this.onSpaceNameDblclick_}">
      ${this.getActiveSpaceName_()}
    </div>
    <div id="sections" tabindex="0" aria-label="Tabs"
        @focusin="${this.onSectionsFocusin_}"
        @focusout="${this.onSectionsFocusout_}"
        @keydown="${this.onSectionsKeydown_}"
        @contextmenu="${this.onEntriesContextmenu_}"
        @pointerover="${this.onSectionsPointerover_}"
        @pointerleave="${this.onSectionsPointerleave_}"
        @pointerdown="${this.onSectionsPointerdown_}"
        @scroll="${this.onSectionsScroll_}"
        @dragstart="${this.onDragstart_}"
        @dragover="${this.onDragover_}" @dragleave="${this.onDragleave_}"
        @drop="${this.onDrop_}" @dragend="${this.onDragend_}"
        @folder-toggle="${this.onFolderToggle_}">
      <div class="section-header" data-flip="pinned-header">
        <span>Pinned</span>
        <button id="newFolder" class="new-folder" data-section="pinned"
            aria-label="New folder" title="New folder"
            @click="${this.onNewFolderClick_}">
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <path d="M8 3v10M3 8h10"></path>
          </svg>
        </button>
      </div>
      <div id="pinned" class="section" role="tree">
        ${this.isPinnedEmpty_() ? html`
          <div class="hint" data-flip="pinned-hint">
            Drag tabs here to pin them
          </div>` : nothing}
        ${repeat(this.getPinned_(), (node: Node) => node.id,
                 (node: Node) => html`
          <compass-sidebar-node .node="${node}" .activeId="${this.activeId_}"
              .profileId="${this.getProfileId_()}">
          </compass-sidebar-node>`)}
      </div>
      <div class="divider" data-flip="divider"></div>
      <div class="section-header" data-flip="today-header">
        <span>Today</span>
        ${this.getToday_().length ? html`
          <button id="clearToday" title="Close all Today tabs"
              @click="${this.onClearTodayClick_}">Clear</button>` : nothing}
        <button id="newTodayFolder" class="new-folder" data-section="today"
            aria-label="New folder in Today" title="New folder"
            @click="${this.onNewFolderClick_}">
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <path d="M8 3v10M3 8h10"></path>
          </svg>
        </button>
        <button id="archiveButton" aria-label="Archived tabs"
            title="Archived tabs" aria-expanded="${this.archiveOpen_}"
            ?open="${this.archiveOpen_}"
            @click="${this.onArchiveButtonClick_}">
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <rect x="2" y="3" width="12" height="3.5" rx="1"></rect>
            <path d="M3 6.5v6a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1v-6M6.5 9h3"></path>
          </svg>
        </button>
      </div>
      <div id="today" class="section" role="tablist">
        ${repeat(this.getToday_(), (node: Node) => node.id,
                 (node: Node) => html`
          <compass-sidebar-node .node="${node}" .activeId="${this.activeId_}"
              .profileId="${this.getProfileId_()}">
          </compass-sidebar-node>`)}
      </div>
    </div>
    <button id="newTab" data-flip="new-tab" @click="${this.onNewTabClick_}">
      <span class="plus">+</span>New Tab
    </button>
  </div>
  ${repeat(this.previews_, (preview: Preview) => preview.space.id,
           (preview: Preview) => html`
    <compass-sidebar-preview side="${preview.side}" aria-hidden="true"
        .space="${preview.space}" .tree="${preview.tree}">
    </compass-sidebar-preview>`)}
</div>
${this.getMenuSpace_() && this.themeOpen_ ? html`
  <div id="spaceMenu" class="theme" role="dialog" aria-label="Theme"
      @keydown="${this.onSpaceMenuKeydown_}">
    <button id="themeBack" title="Back" aria-label="Back"
        @click="${this.onThemeBackClick_}">‹ ${this.getMenuSpace_()!.name}</button>
    <compass-theme-editor .space="${this.getMenuSpace_()}">
    </compass-theme-editor>
  </div>` : nothing}
${this.getMenuSpace_() && !this.themeOpen_ ? html`
  <div id="spaceMenu" role="menu" aria-label="Space"
      @keydown="${this.onSpaceMenuKeydown_}">
    <input id="spaceNameInput" aria-label="Space name" spellcheck="false"
        autocomplete="off"
        .value="${this.getMenuSpace_()!.name}"
        @keydown="${this.onSpaceNameKeydown_}"
        @change="${this.onSpaceNameChange_}">
    <button id="editTheme" role="menuitem" @click="${this.onEditThemeClick_}">
      <span>Edit theme</span>
      <span id="themeChip" style="${this.getSpaceStyle_(this.getMenuSpace_()!)}">
      </span>
    </button>
    <div id="icons" role="group" aria-label="Icon">
      <button class="icon-choice" role="menuitemradio" data-symbol=""
          title="No icon" aria-label="No icon"
          aria-checked="${this.isMenuSpaceIcon_('')}"
          ?selected="${this.isMenuSpaceIcon_('')}"
          @click="${this.onSymbolClick_}">
        ${this.getSpaceInitial_(this.getMenuSpace_()!)}
      </button>
      ${this.spaceSymbols_.map((symbol: SpaceSymbol) => html`
        <button class="icon-choice" role="menuitemradio"
            data-symbol="${symbol.name}" title="${symbol.label}"
            aria-label="${symbol.label}"
            aria-checked="${this.isMenuSpaceIcon_(symbol.name)}"
            ?selected="${this.isMenuSpaceIcon_(symbol.name)}"
            @click="${this.onSymbolClick_}">
          <svg viewBox="${this.getSymbolViewBox_()}" aria-hidden="true">
            <path d="${symbol.path}"></path>
          </svg>
        </button>`)}
      <input id="emojiInput" aria-label="Emoji" placeholder="Emoji"
          spellcheck="false" autocomplete="off"
          title="Emoji (Fn E opens the picker)"
          ?selected="${!!this.getMenuSpace_()!.icon?.emoji}"
          .value="${this.getMenuSpace_()!.icon?.emoji ?? ''}"
          @input="${this.onEmojiInput_}">
    </div>
    <label id="archiveDelay">
      Archive Today tabs after
      <select @change="${this.onArchiveDelayChange_}">
        ${this.archiveDelays_.map((delay: ArchiveDelay) => html`
          <option value="${delay.hours}"
              ?selected="${this.isMenuSpaceArchiveDelay_(delay.hours)}">
            ${delay.label}
          </option>`)}
      </select>
    </label>
    <label id="spaceProfile" ?hidden="${this.profiles_.length === 0}">
      Profile
      <select @change="${this.onSpaceProfileChange_}">
        ${this.profiles_.map((profile: ProfileOption) => html`
          <option value="${profile.id}"
              ?selected="${this.isMenuSpaceProfile_(profile)}">
            ${profile.name}
          </option>`)}
        <option value="">New profile…</option>
      </select>
    </label>
    <button id="deleteSpace" role="menuitem"
        ?disabled="${!this.canDeleteSpace_()}"
        ?confirming="${this.confirmingDelete_}"
        @click="${this.onDeleteSpaceClick_}">
      ${this.getDeleteLabel_()}
    </button>
  </div>` : nothing}
${this.archiveOpen_ ? html`
  <div id="archive" role="dialog" aria-label="Archived tabs"
      @keydown="${this.onArchiveKeydown_}">
    <div class="archive-header">
      <span>Archived</span>
      <button id="clearArchive" ?hidden="${this.isArchiveEmpty_()}"
          @click="${this.onClearArchiveClick_}">Clear</button>
    </div>
    ${this.isArchiveEmpty_() ? html`
      <div class="hint">${this.getArchiveHint_()}</div>` : nothing}
    <div id="archiveList" role="list">
      ${repeat(this.archive_, (entry: ArchivedEntry) => entry.id,
               (entry: ArchivedEntry) => html`
        <div class="archived" role="listitem" data-archived-id="${entry.id}"
            title="${this.getArchivedTitle_(entry)}"
            @click="${this.onArchivedClick_}">
          <img class="favicon" alt=""
              src="${this.getFaviconUrl_(this.getArchivedUrl_(entry))}">
          <span class="title">${this.getArchivedTitle_(entry)}</span>
          <span class="age">${this.getArchivedAge_(entry)}</span>
          <button class="close" data-archived-id="${entry.id}"
              aria-label="Remove from archive"
              @click="${this.onRemoveArchivedClick_}">&times;</button>
        </div>`)}
    </div>
  </div>` : nothing}
<compass-now-playing .items="${this.nowPlaying_}"></compass-now-playing>
<div id="spaces" role="tablist" aria-label="Spaces">
  ${repeat(this.spaces_, (space: Space) => space.id, (space: Space) => html`
    <button class="space" role="tab" data-space-id="${space.id}"
        data-flip="space-${space.id}" draggable="true"
        title="${this.getSpaceTitle_(space)}"
        aria-label="${this.getSpaceTitle_(space)}"
        aria-selected="${this.isActiveSpace_(space)}"
        ?active="${this.isActiveSpace_(space)}"
        @click="${this.onSpaceClick_}"
        @contextmenu="${this.onSpaceContextmenu_}"
        @dragstart="${this.onSpaceDragstart_}"
        @dragend="${this.onDragend_}"
        @dragover="${this.onSpaceDragover_}"
        @dragleave="${this.onSpaceDragleave_}"
        @drop="${this.onSpaceDrop_}">
      ${this.getSpaceSymbol_(space) ? html`
        <svg class="space-symbol" viewBox="${this.getSymbolViewBox_()}"
            aria-hidden="true">
          <path d="${this.getSpaceSymbol_(space)!.path}"></path>
        </svg>` :
        html`<span class="${this.getSpaceIconClass_(space)}">${
            this.getSpaceIconText_(space)}</span>`}
    </button>`)}
  <button id="addSpace" aria-label="New space" title="New space"
      @click="${this.onAddSpaceClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M8 3v10M3 8h10"></path>
    </svg>
  </button>
</div>
<div id="ghosts" aria-hidden="true"></div>
<!--_html_template_end_-->`;
  // clang-format on
}
