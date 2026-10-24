// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {ArchiveDelay, StrataSidebarAppElement} from './app.js';
import type {ArchivedEntry, Node, Space, SpaceColor} from './sidebar.mojom-webui.js';

export function getHtml(this: StrataSidebarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="spaceName" title="Double-click to edit the space"
    @dblclick="${this.onSpaceNameDblclick_}">
  ${this.getActiveSpaceName_()}
</div>
<div id="sections" tabindex="0" aria-label="Tabs"
    @focusin="${this.onSectionsFocusin_}"
    @focusout="${this.onSectionsFocusout_}"
    @keydown="${this.onSectionsKeydown_}"
    @pointerover="${this.onSectionsPointerover_}"
    @pointerleave="${this.onSectionsPointerleave_}"
    @pointerdown="${this.onSectionsPointerdown_}"
    @scroll="${this.onSectionsScroll_}"
    @dragstart="${this.onDragstart_}"
    @dragover="${this.onDragover_}" @dragleave="${this.onDragleave_}"
    @drop="${this.onDrop_}" @dragend="${this.onDragend_}"
    @folder-toggle="${this.onFolderToggle_}">
  <div class="section-header">
    <span>Pinned</span>
    <button id="newFolder" aria-label="New folder" title="New folder"
        @click="${this.onNewFolderClick_}">+</button>
  </div>
  <div id="pinned" class="section" role="tree">
    ${this.isPinnedEmpty_() ? html`
      <div class="hint">Drag tabs here to pin them</div>` : nothing}
    ${repeat(this.getPinned_(), (node: Node) => node.id, (node: Node) => html`
      <strata-sidebar-node .node="${node}" .activeId="${this.activeId_}"
          .collapsedIds="${this.collapsedIds_}">
      </strata-sidebar-node>`)}
  </div>
  <div class="divider"></div>
  <div class="section-header">
    <span>Today</span>
    <button id="archiveButton" aria-label="Archived tabs" title="Archived tabs"
        aria-expanded="${this.archiveOpen_}" ?open="${this.archiveOpen_}"
        @click="${this.onArchiveButtonClick_}">
      <svg viewBox="0 0 16 16" aria-hidden="true">
        <rect x="2" y="3" width="12" height="3.5" rx="1"></rect>
        <path d="M3 6.5v6a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1v-6M6.5 9h3"></path>
      </svg>
    </button>
  </div>
  <div id="today" class="section" role="tablist">
    ${repeat(this.getToday_(), (node: Node) => node.id, (node: Node) => html`
      <strata-sidebar-node .node="${node}" .activeId="${this.activeId_}"
          .collapsedIds="${this.collapsedIds_}">
      </strata-sidebar-node>`)}
  </div>
</div>
<button id="newTab" @click="${this.onNewTabClick_}">
  <span class="plus">+</span>New Tab
</button>
${this.getMenuSpace_() ? html`
  <div id="spaceMenu" role="menu" aria-label="Space"
      @keydown="${this.onSpaceMenuKeydown_}">
    <input id="spaceNameInput" aria-label="Space name"
        .value="${this.getMenuSpace_()!.name}"
        @keydown="${this.onSpaceNameKeydown_}"
        @change="${this.onSpaceNameChange_}">
    <div id="swatches" role="group" aria-label="Color">
      ${this.spaceColors_.map((color: SpaceColor) => html`
        <button class="swatch" role="menuitemradio" data-color="${color}"
            aria-label="${this.getColorName_(color)}"
            title="${this.getColorName_(color)}"
            style="--swatch-color: ${this.getSpaceColor_(color)}"
            aria-checked="${this.isMenuSpaceColor_(color)}"
            ?selected="${this.isMenuSpaceColor_(color)}"
            @click="${this.onSwatchClick_}">
        </button>`)}
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
<div id="spaces" role="tablist" aria-label="Spaces">
  ${repeat(this.spaces_, (space: Space) => space.id, (space: Space) => html`
    <button class="space" role="tab" data-space-id="${space.id}"
        title="${space.name}" aria-label="${space.name}"
        style="--space-color: ${this.getSpaceColor_(space.color)}"
        aria-selected="${this.isActiveSpace_(space)}"
        ?active="${this.isActiveSpace_(space)}"
        @click="${this.onSpaceClick_}"
        @contextmenu="${this.onSpaceContextmenu_}"
        @dragover="${this.onSpaceDragover_}"
        @dragleave="${this.onSpaceDragleave_}"
        @drop="${this.onSpaceDrop_}">
      ${this.getSpaceInitial_(space)}
    </button>`)}
  <button id="addSpace" aria-label="New space" title="New space"
      @click="${this.onAddSpaceClick_}">+</button>
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
