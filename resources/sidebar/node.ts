// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from './browser_proxy.js';
import {getHtml} from './node.html.js';
import type {Node} from './sidebar.mojom-webui.js';
import {NodeType} from './sidebar.mojom-webui.js';

// One sidebar entry: a tab, a split (its panes in a row) or a folder (its
// children, indented). Renders into the light DOM, so the app's styles and
// drag and drop handling apply to the rows of the whole tree.
//
// Entries carry the `row` class and a `data-node-id`; the app finds drop
// targets by them. A split's panes aren't rows: a split moves as one entry.
// Tabs (panes included) and folder rows take keyboard focus; the app moves it
// between them.
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
      collapsedIds: {type: Object, hasChanged: () => true},
      editing_: {type: Boolean, state: true},
    };
  }

  accessor node: Node|null = null;
  accessor activeId: string|null = null;
  // Folders the user collapsed. Owned by the app.
  accessor collapsedIds: Set<string> = new Set();
  protected accessor editing_: boolean = false;

  protected isTab_(): boolean {
    return this.node?.type === NodeType.kTab;
  }

  protected isSplit_(): boolean {
    return this.node?.type === NodeType.kSplit;
  }

  protected isFolder_(): boolean {
    return this.node?.type === NodeType.kFolder;
  }

  protected isActive_(node: Node): boolean {
    return node.id === this.activeId;
  }

  protected isCollapsed_(): boolean {
    return !!this.node && this.collapsedIds.has(this.node.id);
  }

  protected getChildren_(): Node[] {
    return this.node?.children ?? [];
  }

  protected getTitle_(node: Node): string {
    return node.title || node.url || 'New Tab';
  }

  protected getCloseLabel_(node: Node): string {
    return node.live ? 'Close tab' : 'Remove';
  }

  protected getFaviconUrl_(node: Node): string {
    const params = new URLSearchParams({
      size: '16',
      scaleFactor: `${window.devicePixelRatio}x`,
      pageUrl: node.url,
      allowGoogleServerFallback: '0',
    });
    return `chrome://favicon2/?${params.toString()}`;
  }

  protected getNodeId_(e: Event): string {
    return (e.currentTarget as HTMLElement).dataset['nodeId']!;
  }

  protected onTabClick_(e: Event) {
    SidebarBrowserProxy.getInstance().handler.activateNode(this.getNodeId_(e));
  }

  protected onCloseClick_(e: Event) {
    e.stopPropagation();
    SidebarBrowserProxy.getInstance().handler.closeNode(this.getNodeId_(e));
  }

  protected onFolderClick_() {
    this.fire('folder-toggle', this.node!.id);
  }

  protected onDeleteFolderClick_(e: Event) {
    e.stopPropagation();
    SidebarBrowserProxy.getInstance().handler.deleteNode(this.node!.id);
  }

  protected onFolderTitleDblclick_(e: Event) {
    e.stopPropagation();
    this.startRename();
  }

  // Shows a folder's name as an input, selected.
  async startRename() {
    if (!this.isFolder_()) {
      return;
    }
    this.editing_ = true;
    await this.updateComplete;
    const input = this.querySelector<HTMLInputElement>('.folder-title-input');
    input?.select();
  }

  protected onTitleInputClick_(e: Event) {
    // Clicking into the name shouldn't collapse the folder.
    e.stopPropagation();
  }

  protected onTitleKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter') {
      this.commitRename_(e.target as HTMLInputElement);
    } else if (e.key === 'Escape') {
      this.editing_ = false;
      this.updateComplete.then(() => {
        this.querySelector<HTMLElement>('.folder-row')?.focus();
      });
    }
  }

  protected onTitleBlur_(e: Event) {
    this.commitRename_(e.target as HTMLInputElement);
  }

  private commitRename_(input: HTMLInputElement) {
    if (!this.editing_) {
      return;
    }
    // Enter leaves the focus on the row; a blur leaves it where it went.
    const hadFocus = input.matches(':focus');
    this.editing_ = false;
    if (hadFocus) {
      this.updateComplete.then(() => {
        this.querySelector<HTMLElement>('.folder-row')?.focus();
      });
    }
    const title = input.value.trim();
    if (title && title !== this.node!.title) {
      SidebarBrowserProxy.getInstance().handler.renameFolder(
          this.node!.id, title);
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-node': StrataSidebarNodeElement;
  }
}

customElements.define(StrataSidebarNodeElement.is, StrataSidebarNodeElement);
