// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {StrataSidebarAppElement} from './app.js';
import type {Node, Space, SpaceColor} from './sidebar.mojom-webui.js';

export function getHtml(this: StrataSidebarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="spaceName" title="Double-click to edit the space"
    @dblclick="${this.onSpaceNameDblclick_}">
  ${this.getActiveSpaceName_()}
</div>
<div id="sections" @dragstart="${this.onDragstart_}"
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
    <button id="deleteSpace" role="menuitem"
        ?disabled="${!this.canDeleteSpace_()}"
        ?confirming="${this.confirmingDelete_}"
        @click="${this.onDeleteSpaceClick_}">
      ${this.getDeleteLabel_()}
    </button>
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
