// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import './node.js';
import {getCss as getPreviewCss} from './preview.css.js';
import {getHtml} from './preview.html.js';
import type {Node, Space, Tree} from './sidebar.mojom-webui.js';

// Another space's entries, as the sidebar would show them: what a swipe pulls
// in before the window switches (docs/design/spaces.md, "Swiping"). Only to
// look at. Its own shadow root keeps its rows out of the app's queries.
export class CompassSidebarPreviewElement extends CrLitElement {
  static get is() {
    return 'compass-sidebar-preview';
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
      entriesScrollTop: {type: Number},
    };
  }

  accessor space: Space|null = null;
  accessor tree: Tree|null = null;
  // Where the space's entries were scrolled when the window left it: where
  // the switch will show them.
  accessor entriesScrollTop: number = 0;

  override updated(changed: PropertyValues<this>) {
    super.updated(changed);
    if (changed.has('tree') || changed.has('entriesScrollTop')) {
      // Once the entries' own rows have rendered, which they do after this.
      requestAnimationFrame(() => {
        const sections =
            this.shadowRoot.querySelector<HTMLElement>('#sections');
        if (sections) {
          sections.scrollTop = this.entriesScrollTop;
        }
      });
    }
  }

  protected getName_(): string {
    return this.space?.name ?? '';
  }

  protected getPinned_(): Node[] {
    return this.tree?.pinned.children ?? [];
  }

  protected getToday_(): Node[] {
    return this.tree?.today.children ?? [];
  }

  // The entry the switch would show (the browser's guess), if any: a space
  // with nothing open shows the empty tab, which has no entry.
  protected getActiveId_(): string|null {
    return this.tree?.activeId ?? null;
  }

  protected isPinnedEmpty_(): boolean {
    return !!this.tree && this.getPinned_().length === 0;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-sidebar-preview': CompassSidebarPreviewElement;
  }
}

customElements.define(
    CompassSidebarPreviewElement.is, CompassSidebarPreviewElement);
