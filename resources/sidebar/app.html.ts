// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSidebarAppElement, Preview} from './app.js';
import {speakerIcon} from './speaker_icon.js';
import type {ArchivedEntry, Node, RoomNotice, Space} from './sidebar.mojom-webui.js';

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
      ${node.badgeCount ? html`
        <span class="tile-badge" role="status"
            aria-label="${node.badgeCount < 0 ? 'New notifications' :
                          `${node.badgeCount} notifications`}"></span>` :
        nothing}
      ${this.hasAudio_(node) ? html`
        <button class="audio" data-node-id="${node.id}"
            ?muted="${this.isMuted_(node)}"
            aria-label="${this.getAudioLabel_(node)}"
            title="${this.getAudioLabel_(node)}"
            @click="${this.onTileAudioClick_}">
          ${speakerIcon(this.isMuted_(node))}
        </button>` : nothing}
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
      ${this.roomNotices_.map((notice: RoomNotice) => html`
        <div class="room-notice" role="status">
          <span>${notice.text}</span>
          <button aria-label="Dismiss" data-room-id="${notice.roomId}"
              @click="${this.onDismissRoomNoticeClick_}">&times;</button>
        </div>`)}
      ${this.hasPinned_ ? html`
        <div class="section-header" data-flip="pinned-header">
          <span>Pinned</span>
          <button id="newLiveFolder" class="new-folder"
              aria-label="New live folder" title="New live folder"
              @click="${this.onNewLiveFolderClick_}">
            <svg viewBox="0 0 16 16" aria-hidden="true">
              <path d="M2 4.2a.7.7 0 0 1 .7-.7h3.1l1.4 1.5h6.1a.7.7 0 0 1 .7.7
                       v6.6a.7.7 0 0 1-.7.7H2.7a.7.7 0 0 1-.7-.7z
                       M8.7 6.6 7.2 9h1.8l-1.5 2.4"></path>
            </svg>
          </button>
          <button id="newEasel" class="new-folder"
              aria-label="New Easel" title="New Easel"
              @click="${this.onNewEaselClick_}">
            <svg viewBox="0 0 16 16" aria-hidden="true">
              <rect x="2.5" y="2.5" width="11" height="7.5" rx="1"></rect>
              <path d="M5.5 10 3.8 14M10.5 10l1.7 4M8 10v3.5M8 1v1.5"></path>
            </svg>
          </button>
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
                .profileId="${this.getProfileId_()}" pinned>
            </compass-sidebar-node>`)}
        </div>
        <div class="divider" data-flip="divider"></div>
      ` : nothing}
      <div class="section-header" data-flip="today-header">
        <span>Today</span>
        ${this.tidyAvailable_ ? html`
          <button id="tidyToday" ?busy="${this.tidyBusy_}"
              ?disabled="${!this.canTidy_()}"
              title="${this.tidyBusy_ ? 'Claude is tidying Today' :
                  'Group Today\'s tabs into folders with Claude'}"
              @click="${this.onTidyClick_}">${
              this.tidyBusy_ ? 'Tidying\u2026' : 'Tidy'}</button>` : nothing}
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
      <div id="today" class="section" role="tablist"
          ?empty="${this.isTodayEmpty_()}"
          ?dragging="${this.dragActive_ || this.linkDragging_}">
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
        .space="${preview.space}" .tree="${preview.tree}"
        .entriesScrollTop="${preview.scrollTop}">
    </compass-sidebar-preview>`)}
  ${this.toast_ ? html`
    <div id="toast" role="status">
      <span class="text">${this.toast_.text}</span>
      ${this.toast_.undoTidyId ? html`
        <button id="toastUndo" @click="${this.onToastUndoClick_}">Undo</button>
      ` : nothing}
      <button id="toastClose" aria-label="Dismiss" title="Dismiss"
          @click="${this.onToastCloseClick_}">&times;</button>
    </div>` : nothing}
</div>
${this.liveEditor_ ? html`
  <compass-live-folder-editor .parentId="${this.liveEditor_.parentId}"
      .folderId="${this.liveEditor_.folderId}"
      @close="${this.onLiveEditorClose_}">
  </compass-live-folder-editor>` : nothing}
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
        ?local-only="${!space.synced}"
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
