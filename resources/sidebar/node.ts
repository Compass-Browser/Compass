// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from './browser_proxy.js';
import {getFaviconUrl} from './favicon.js';
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
      // Whose icons the entries show: the tree's profile.
      profileId: {type: String},
      editingId_: {type: String, state: true},
    };
  }

  accessor node: Node|null = null;
  accessor activeId: string|null = null;
  accessor profileId: string|null = null;
  // The node whose name shows as an input: this folder or tab, or a pane.
  protected accessor editingId_: string|null = null;

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
    return !!this.node?.collapsed;
  }

  protected getChildren_(): Node[] {
    return this.node?.children ?? [];
  }

  protected getTitle_(node: Node): string {
    return node.customTitle || node.title || node.url || 'New Tab';
  }

  protected isEditing_(node: Node): boolean {
    return node.id === this.editingId_;
  }

  protected getCloseLabel_(node: Node): string {
    return node.live ? 'Close tab' : 'Remove';
  }

  protected getFaviconUrl_(node: Node): string {
    return getFaviconUrl(node.url, 16, this.profileId);
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
    this.startRename(this.node!.id);
  }

  // As in Arc, double-clicking a tab names it.
  protected onTabDblclick_(e: Event) {
    e.stopPropagation();
    this.startRename(this.getNodeId_(e));
  }

  // Shows the name of this folder or tab, or of pane |id|, as an input,
  // selected.
  async startRename(id: string) {
    const node = this.node?.id === id ?
        this.node :
        this.getChildren_().find(pane => pane.id === id && this.isSplit_());
    if (!node || node.type === NodeType.kSplit) {
      return;
    }
    this.editingId_ = id;
    await this.updateComplete;
    this.querySelector<HTMLInputElement>('.title-input')?.select();
  }

  protected onTitleInputClick_(e: Event) {
    // Clicking into the name shouldn't collapse the folder or activate the
    // tab.
    e.stopPropagation();
  }

  protected onTitleInputDblclick_(e: Event) {
    // Selecting a word in the name shouldn't start another rename.
    e.stopPropagation();
  }

  protected onTitleKeydown_(e: KeyboardEvent) {
    // Keys typed into the name aren't the list's.
    e.stopPropagation();
    if (e.key === 'Enter') {
      this.commitRename_(e.target as HTMLInputElement);
    } else if (e.key === 'Escape') {
      const id = this.editingId_;
      this.editingId_ = null;
      this.updateComplete.then(() => this.focusRow_(id));
    }
  }

  private focusRow_(id: string|null) {
    this.querySelector<HTMLElement>(
            `.folder-row[data-node-id="${id}"], .tab[data-node-id="${id}"]`)
        ?.focus();
  }

  protected onTitleBlur_(e: Event) {
    this.commitRename_(e.target as HTMLInputElement);
  }

  // A folder needs a name; a tab without one goes back to its page's title.
  private commitRename_(input: HTMLInputElement) {
    const id = this.editingId_;
    const node = this.node?.id === id ?
        this.node :
        this.getChildren_().find(pane => pane.id === id);
    if (!id || !node) {
      return;
    }
    // Enter leaves the focus on the row; a blur leaves it where it went.
    const hadFocus = input.matches(':focus');
    this.editingId_ = null;
    if (hadFocus) {
      this.updateComplete.then(() => this.focusRow_(id));
    }
    const title = input.value.trim();
    const handler = SidebarBrowserProxy.getInstance().handler;
    if (node.type === NodeType.kFolder) {
      if (title && title !== node.title) {
        handler.renameFolder(id, title);
      }
    } else if (title !== this.getTitle_(node)) {
      handler.renameTab(id, title === node.title ? '' : title);
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-node': StrataSidebarNodeElement;
  }
}

customElements.define(StrataSidebarNodeElement.is, StrataSidebarNodeElement);
