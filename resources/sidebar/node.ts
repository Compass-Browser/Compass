// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from './browser_proxy.js';
import {getHtml} from './node.html.js';
import type {Node} from './sidebar.mojom-webui.js';
import {NodeType} from './sidebar.mojom-webui.js';

// One sidebar entry: a tab, a split (its panes in a row) or a folder (its
// children, indented). Renders into the light DOM, so the app's styles apply
// and the rows of the whole tree sit in one shadow root.
export class StrataSidebarNodeElement extends CrLitElement {
  static get is() {
    return 'strata-sidebar-node';
  }

  override render() {
    return getHtml.bind(this)();
  }

  override createRenderRoot() {
    return this;
  }

  static override get properties() {
    return {
      // The app updates nodes in place, so a render of the parent always
      // re-renders its children.
      node: {type: Object, hasChanged: () => true},
      activeId: {type: String},
    };
  }

  accessor node: Node|null = null;
  accessor activeId: string|null = null;

  protected isTab_(): boolean {
    return this.node?.type === NodeType.kTab;
  }

  protected isSplit_(): boolean {
    return this.node?.type === NodeType.kSplit;
  }

  protected isFolder_(): boolean {
    return this.node?.type === NodeType.kFolder;
  }

  protected isActive_(): boolean {
    return !!this.node && this.node.id === this.activeId;
  }

  protected getChildren_(): Node[] {
    return this.node?.children ?? [];
  }

  protected getTitle_(): string {
    return this.node?.title || this.node?.url || 'New Tab';
  }

  protected getFaviconUrl_(): string {
    const params = new URLSearchParams({
      size: '16',
      scaleFactor: `${window.devicePixelRatio}x`,
      pageUrl: this.node?.url ?? '',
      allowGoogleServerFallback: '0',
    });
    return `chrome://favicon2/?${params.toString()}`;
  }

  protected onTabClick_() {
    SidebarBrowserProxy.getInstance().handler.activateNode(this.node!.id);
  }

  protected onCloseClick_(e: Event) {
    e.stopPropagation();
    SidebarBrowserProxy.getInstance().handler.closeNode(this.node!.id);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-node': StrataSidebarNodeElement;
  }
}

customElements.define(StrataSidebarNodeElement.is, StrataSidebarNodeElement);
