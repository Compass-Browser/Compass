// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSidebarNodeElement} from './node.js';
import type {Node} from './sidebar.mojom-webui.js';

// A speaker, with sound waves, or crossed out when |muted|. Tab rows and
// favorites' tiles show it on tabs that play sound. Whole <svg>s: lit can't
// put a nested template's paths in the SVG namespace.
export function speakerIcon(muted: boolean) {
  // clang-format off
  return muted ? html`
    <svg class="speaker-icon" viewBox="0 0 16 16" aria-hidden="true">
      <path class="speaker-body" d="M2.5 6h2.5l3.5-3v10l-3.5-3h-2.5z"></path>
      <path class="speaker-line" d="M11 6l4 4M15 6l-4 4"></path>
    </svg>` : html`
    <svg class="speaker-icon" viewBox="0 0 16 16" aria-hidden="true">
      <path class="speaker-body" d="M2.5 6h2.5l3.5-3v10l-3.5-3h-2.5z"></path>
      <path class="speaker-line" d="M10.75 5.75a3 3 0 0 1 0 4.5"></path>
      <path class="speaker-line" d="M12.5 3.75a6 6 0 0 1 0 8.5"></path>
    </svg>`;
  // clang-format on
}

export function getHtml(this: CompassSidebarNodeElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isTab_() ? html`
  <div class="tab row" role="tab" draggable="true" tabindex="-1"
      data-node-id="${this.node!.id}"
      aria-selected="${this.isActive_(this.node!)}"
      ?active="${this.isActive_(this.node!)}" ?unloaded="${!this.node!.live}"
      title="${this.getTitle_(this.node!)}" @click="${this.onTabClick_}"
      @dblclick="${this.onTabDblclick_}">
    <img class="favicon" src="${this.getFaviconUrl_(this.node!)}" alt="">
    ${this.isEditing_(this.node!) ? html`
      <input class="title-input tab-title-input"
          .value="${this.getTitle_(this.node!)}" aria-label="Tab name"
          @click="${this.onTitleInputClick_}"
          @dblclick="${this.onTitleInputDblclick_}"
          @keydown="${this.onTitleKeydown_}" @blur="${this.onTitleBlur_}">` : html`
      <span class="title">${this.getTitle_(this.node!)}</span>`}
    ${this.hasAudio_(this.node!) ? html`
      <button class="audio" data-node-id="${this.node!.id}"
          ?muted="${this.isMuted_(this.node!)}"
          aria-label="${this.getAudioLabel_(this.node!)}"
          title="${this.getAudioLabel_(this.node!)}"
          @click="${this.onAudioClick_}" @dblclick="${this.onAudioDblclick_}">
        ${speakerIcon(this.isMuted_(this.node!))}
      </button>` : nothing}
    <button class="close" data-node-id="${this.node!.id}"
        aria-label="${this.getCloseLabel_(this.node!)}"
        @click="${this.onCloseClick_}">&times;</button>
  </div>` : nothing}
${this.isSplit_() ? html`
  <div class="split row" draggable="true" data-node-id="${this.node!.id}">
    ${repeat(this.getChildren_(), (pane: Node) => pane.id, (pane: Node) => html`
      <div class="tab pane" role="tab" tabindex="-1"
          data-node-id="${pane.id}"
          aria-selected="${this.isActive_(pane)}"
          ?active="${this.isActive_(pane)}" ?unloaded="${!pane.live}"
          title="${this.getTitle_(pane)}" @click="${this.onTabClick_}"
          @dblclick="${this.onTabDblclick_}">
        <img class="favicon" src="${this.getFaviconUrl_(pane)}" alt="">
        ${this.isEditing_(pane) ? html`
          <input class="title-input tab-title-input"
              .value="${this.getTitle_(pane)}" aria-label="Tab name"
              @click="${this.onTitleInputClick_}"
              @dblclick="${this.onTitleInputDblclick_}"
              @keydown="${this.onTitleKeydown_}" @blur="${this.onTitleBlur_}">` : html`
          <span class="title">${this.getTitle_(pane)}</span>`}
        ${this.hasAudio_(pane) ? html`
          <button class="audio" data-node-id="${pane.id}"
              ?muted="${this.isMuted_(pane)}"
              aria-label="${this.getAudioLabel_(pane)}"
              title="${this.getAudioLabel_(pane)}"
              @click="${this.onAudioClick_}"
              @dblclick="${this.onAudioDblclick_}">
            ${speakerIcon(this.isMuted_(pane))}
          </button>` : nothing}
        <button class="close" data-node-id="${pane.id}"
            aria-label="${this.getCloseLabel_(pane)}"
            @click="${this.onCloseClick_}">&times;</button>
      </div>`)}
  </div>` : nothing}
${this.isFolder_() ? html`
  <div class="folder-row row" draggable="true" data-node-id="${this.node!.id}"
      role="treeitem" tabindex="-1" aria-expanded="${!this.isCollapsed_()}"
      @click="${this.onFolderClick_}">
    <span class="chevron" ?collapsed="${this.isCollapsed_()}"></span>
    ${this.isEditing_(this.node!) ? html`
      <input class="title-input folder-title-input" .value="${this.node!.title}"
          aria-label="Folder name" @click="${this.onTitleInputClick_}"
          @keydown="${this.onTitleKeydown_}" @blur="${this.onTitleBlur_}">` : html`
      <span class="title" @dblclick="${this.onFolderTitleDblclick_}">
        ${this.node!.title}
      </span>`}
    <button class="close" aria-label="Delete folder"
        @click="${this.onDeleteFolderClick_}">&times;</button>
  </div>
  ${!this.isCollapsed_() ? html`
    <div class="folder-children" role="group">
      ${repeat(this.getChildren_(), (child: Node) => child.id,
               (child: Node) => html`
        <compass-sidebar-node .node="${child}" .activeId="${this.activeId}"
            .profileId="${this.profileId}">
        </compass-sidebar-node>`)}
    </div>` : nothing}` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
