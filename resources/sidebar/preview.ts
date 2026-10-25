// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import './node.js';
import {getCss as getPreviewCss} from './preview.css.js';
import {getHtml} from './preview.html.js';
import type {Node, Space, Tree} from './sidebar.mojom-webui.js';
import {NodeType} from './sidebar.mojom-webui.js';

// What a space with no entries shows once the window switches to it: the new
// tab the browser opens there (docs/design/spaces.md, "Switching, step by
// step"), active.
const NEW_TAB: Node = {
  id: 'preview-new-tab',
  type: NodeType.kTab,
  title: 'New tab',
  url: 'chrome://newtab/',
  live: true,
  children: [],
};

// Another space's entries, as the sidebar would show them: what a swipe pulls
// in before the window switches (docs/design/spaces.md, "Swiping"). Only to
// look at. Its own shadow root keeps its rows out of the app's queries.
export class StrataSidebarPreviewElement extends CrLitElement {
  static get is() {
    return 'strata-sidebar-preview';
  }

  static override get styles() {
    return [getCss(), getPreviewCss()];
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      space: {type: Object},
      // Null until it arrives: only the name shows.
      tree: {type: Object},
      collapsedIds: {type: Object},
    };
  }

  accessor space: Space|null = null;
  accessor tree: Tree|null = null;
  accessor collapsedIds: Set<string> = new Set();

  protected getName_(): string {
    return this.space?.name ?? '';
  }

  protected getPinned_(): Node[] {
    return this.tree?.pinned.children ?? [];
  }

  protected getToday_(): Node[] {
    if (this.tree && this.tree.pinned.children.length === 0 &&
        this.tree.today.children.length === 0) {
      return [NEW_TAB];
    }
    return this.tree?.today.children ?? [];
  }

  // The entry the switch would show: the browser's guess, or the new tab.
  protected getActiveId_(): string|null {
    return this.getToday_()[0] === NEW_TAB ? NEW_TAB.id :
                                             this.tree?.activeId ?? null;
  }

  protected isPinnedEmpty_(): boolean {
    return !!this.tree && this.getPinned_().length === 0;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-preview': StrataSidebarPreviewElement;
  }
}

customElements.define(
    StrataSidebarPreviewElement.is, StrataSidebarPreviewElement);
