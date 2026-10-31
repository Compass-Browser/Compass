// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSidebarPreviewElement} from './preview.js';
import type {Node} from './sidebar.mojom-webui.js';

// The live pane's markup (app.html.ts), without its handlers.
export function getHtml(this: CompassSidebarPreviewElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="spaceName">${this.getName_()}</div>
${this.tree ? html`
  <div id="sections">
    <div class="section-header">
      <span>Pinned</span>
      <span id="newFolder" class="new-folder">+</span>
    </div>
    <div class="section">
      ${this.isPinnedEmpty_() ? html`
        <div class="hint">Drag tabs here to pin them</div>` : nothing}
      ${repeat(this.getPinned_(), (node: Node) => node.id, (node: Node) => html`
        <compass-sidebar-node .node="${node}" .activeId="${this.getActiveId_()}"
            .profileId="${this.space?.profileId ?? null}">
        </compass-sidebar-node>`)}
    </div>
    <div class="divider"></div>
    <div class="section-header">
      <span>Today</span>
      <span id="archiveButton">
        <svg viewBox="0 0 16 16" aria-hidden="true">
          <rect x="2" y="3" width="12" height="3.5" rx="1"></rect>
          <path d="M3 6.5v6a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1v-6M6.5 9h3"></path>
        </svg>
      </span>
    </div>
    <div class="section">
      ${repeat(this.getToday_(), (node: Node) => node.id, (node: Node) => html`
        <compass-sidebar-node .node="${node}" .activeId="${this.getActiveId_()}"
            .profileId="${this.space?.profileId ?? null}">
        </compass-sidebar-node>`)}
    </div>
  </div>
  <div id="newTab"><span class="plus">+</span>New Tab</div>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
