// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {StrataSidebarAppElement} from './app.js';
import type {Node} from './sidebar.mojom-webui.js';

export function getHtml(this: StrataSidebarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
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
<!--_html_template_end_-->`;
  // clang-format on
}
