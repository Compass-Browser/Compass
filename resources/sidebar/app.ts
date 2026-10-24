// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {SidebarBrowserProxy} from './browser_proxy.js';
import './node.js';
import type {Node, Tree} from './sidebar.mojom-webui.js';
import {NodeType} from './sidebar.mojom-webui.js';

// Where a dragged entry, or a dropped link, would land: the slot |index| among |parent|'s
// current children. |target| shows the drop indicator, as its `drop`
// attribute.
interface DropTarget {
  parent: Node;
  index: number;
  target: HTMLElement;
  position: 'before'|'after'|'into'|'end';
}

// The sidebar: the window's SpaceModel, as a pinned and a Today section. The
// browser sends the tree once, then diffs. Entries are rearranged by dragging,
// and links dragged in open where they're dropped. Slices b, c and e of
// docs/design/sidebar.md.
export class StrataSidebarAppElement extends CrLitElement {
  static get is() {
    return 'strata-sidebar-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      pinned_: {type: Object},
      today_: {type: Object},
      activeId_: {type: String},
      collapsedIds_: {type: Object},
    };
  }

  protected accessor pinned_: Node|null = null;
  protected accessor today_: Node|null = null;
  protected accessor activeId_: string|null = null;
  protected accessor collapsedIds_: Set<string> = new Set();

  private browserProxy_: SidebarBrowserProxy =
      SidebarBrowserProxy.getInstance();
  private listenerIds_: number[] = [];

  // Every node of the tree, by ID, with its parent.
  private nodes_: Map<string, Node> = new Map();
  private parents_: Map<string, Node> = new Map();

  private draggingId_: string|null = null;
  private dropTarget_: DropTarget|null = null;

  override connectedCallback() {
    super.connectedCallback();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.setTree.addListener(this.onSetTree_.bind(this)),
      router.nodeAdded.addListener(this.onNodeAdded_.bind(this)),
      router.nodeMoved.addListener(this.onNodeMoved_.bind(this)),
      router.nodeChanged.addListener(this.onNodeChanged_.bind(this)),
      router.nodeRemoved.addListener(this.onNodeRemoved_.bind(this)),
      router.nodeLiveChanged.addListener(this.onNodeLiveChanged_.bind(this)),
      router.activeNodeChanged.addListener(
          (id: string|null) => this.activeId_ = id),
    ];
    // Diffs that arrive before the tree are about nodes the page doesn't know
    // yet, and the lookups below drop them. The tree already has them.
    this.browserProxy_.handler.requestTree();
    document.addEventListener('dragover', this.onDocumentDragover_);
    document.addEventListener('drop', this.onDocumentDrop_);
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    document.removeEventListener('dragover', this.onDocumentDragover_);
    document.removeEventListener('drop', this.onDocumentDrop_);
    for (const id of this.listenerIds_) {
      this.browserProxy_.callbackRouter.removeListener(id);
    }
    this.listenerIds_ = [];
  }

  protected getPinned_(): Node[] {
    return this.pinned_?.children ?? [];
  }

  protected getToday_(): Node[] {
    return this.today_?.children ?? [];
  }

  protected isPinnedEmpty_(): boolean {
    return this.getPinned_().length === 0;
  }

  protected onNewTabClick_() {
    this.browserProxy_.handler.newTab();
  }

  protected onNewFolderClick_() {
    if (this.pinned_) {
      this.browserProxy_.handler.addFolder(
          this.pinned_.id, this.pinned_.children.length, 'New Folder');
    }
  }

  protected onFolderToggle_(e: CustomEvent<string>) {
    const collapsed = new Set(this.collapsedIds_);
    if (!collapsed.delete(e.detail)) {
      collapsed.add(e.detail);
    }
    this.collapsedIds_ = collapsed;
  }

  protected onDragstart_(e: DragEvent) {
    const row = (e.target as Element).closest<HTMLElement>('.row');
    if (!row || !e.dataTransfer) {
      return;
    }
    this.draggingId_ = row.dataset['nodeId']!;
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData('text/plain', this.draggingId_);
    row.setAttribute('dragging', '');
  }

  protected onDragover_(e: DragEvent) {
    const drop = this.findDropTarget_(e);
    this.showDropTarget_(drop);
    if (drop) {
      e.preventDefault();
      e.dataTransfer!.dropEffect = this.draggingId_ ? 'move' : 'copy';
    }
  }

  // A drop the page doesn't take would navigate the sidebar to it, so the
  // document takes every drag; only drop targets accept one.
  private onDocumentDragover_ = (e: DragEvent) => {
    if (!e.defaultPrevented) {
      e.preventDefault();
      e.dataTransfer!.dropEffect = 'none';
    }
  };

  private onDocumentDrop_ = (e: DragEvent) => {
    e.preventDefault();
  };

  protected onDragleave_(e: DragEvent) {
    const sections = e.currentTarget as HTMLElement;
    if (!sections.contains(e.relatedTarget as globalThis.Node | null)) {
      this.showDropTarget_(null);
    }
  }

  protected onDrop_(e: DragEvent) {
    e.preventDefault();
    const drop = this.dropTarget_;
    if (drop && this.draggingId_) {
      this.browserProxy_.handler.moveNode(
          this.draggingId_, drop.parent.id, drop.index);
    } else if (drop) {
      // The first URL of the list; lines starting with # are comments.
      const url = e.dataTransfer!.getData('text/uri-list')
                      .split(/\r?\n/)
                      .find(line => line && !line.startsWith('#'));
      if (url) {
        this.browserProxy_.handler.openLink(url, drop.parent.id, drop.index);
      }
    }
    this.endDrag_();
  }

  protected onDragend_() {
    this.endDrag_();
  }

  private endDrag_() {
    this.showDropTarget_(null);
    this.shadowRoot.querySelector('[dragging]')?.removeAttribute('dragging');
    this.draggingId_ = null;
  }

  private showDropTarget_(drop: DropTarget|null) {
    if (this.dropTarget_?.target !== drop?.target ||
        this.dropTarget_?.position !== drop?.position) {
      this.dropTarget_?.target.removeAttribute('drop');
      drop?.target.setAttribute('drop', drop.position);
    }
    this.dropTarget_ = drop;
  }

  // Tab rows take drops before (top half) or after (bottom half) them; folder
  // rows also take drops into them (middle third). Anywhere else in a section
  // drops at its end. Returns null where the dragged entry (or link) can't
  // go; the browser checks again.
  private findDropTarget_(e: DragEvent): DropTarget|null {
    const dragged =
        this.draggingId_ ? this.nodes_.get(this.draggingId_) ?? null : null;
    const isLink = !this.draggingId_ &&
        !!e.dataTransfer?.types.includes('text/uri-list');
    if (!dragged && !isLink) {
      return null;
    }
    const element = e.target as Element;
    let drop: DropTarget|null = null;
    const row = element.closest<HTMLElement>('.row');
    if (row) {
      const node = this.nodes_.get(row.dataset['nodeId']!);
      const parent = node && this.parents_.get(node.id);
      if (!node || !parent) {
        return null;
      }
      const rect = row.getBoundingClientRect();
      const y = (e.clientY - rect.top) / rect.height;
      if (node.type === NodeType.kFolder && y > 1 / 3 && y < 2 / 3) {
        drop = {
          parent: node,
          index: node.children.length,
          target: row,
          position: 'into',
        };
      } else {
        const after = y >= 0.5;
        drop = {
          parent,
          index: parent.children.indexOf(node) + (after ? 1 : 0),
          target: row,
          position: after ? 'after' : 'before',
        };
      }
    } else {
      const section = element.closest<HTMLElement>('.section');
      const root = section?.id === 'pinned' ? this.pinned_ : this.today_;
      if (!section || !root) {
        return null;
      }
      drop = {
        parent: root,
        index: root.children.length,
        target: section,
        position: 'end',
      };
    }
    return this.canDrop_(dragged, drop.parent) ? drop : null;
  }

  // Mirrors SpaceModel::CanMove() for the cases the page can see. A link
  // (|dragged| null) goes wherever a new tab can.
  private canDrop_(dragged: Node|null, parent: Node): boolean {
    for (let node: Node|undefined = parent; node;
         node = this.parents_.get(node.id)) {
      if (node === dragged) {
        return false;
      }
    }
    if (parent.type === NodeType.kSplit) {
      return false;
    }
    if (dragged?.type === NodeType.kFolder) {
      return parent === this.pinned_ || this.isInPinned_(parent);
    }
    return true;
  }

  private isInPinned_(node: Node): boolean {
    for (let n: Node|undefined = node; n; n = this.parents_.get(n.id)) {
      if (n === this.pinned_) {
        return true;
      }
    }
    return false;
  }

  private index_(node: Node, parent: Node|null) {
    this.nodes_.set(node.id, node);
    if (parent) {
      this.parents_.set(node.id, parent);
    }
    for (const child of node.children) {
      this.index_(child, node);
    }
  }

  private unindex_(node: Node) {
    this.nodes_.delete(node.id);
    this.parents_.delete(node.id);
    for (const child of node.children) {
      this.unindex_(child);
    }
  }

  private onSetTree_(tree: Tree) {
    this.nodes_.clear();
    this.parents_.clear();
    this.index_(tree.pinned, null);
    this.index_(tree.today, null);
    this.pinned_ = tree.pinned;
    this.today_ = tree.today;
    this.activeId_ = tree.activeId;
  }

  private onNodeAdded_(parentId: string, index: number, node: Node) {
    const parent = this.nodes_.get(parentId);
    if (!parent || this.nodes_.has(node.id)) {
      return;
    }
    parent.children.splice(index, 0, node);
    this.index_(node, parent);
    this.requestUpdate();
  }

  private onNodeMoved_(id: string, newParentId: string, index: number) {
    const node = this.nodes_.get(id);
    const oldParent = this.parents_.get(id);
    const newParent = this.nodes_.get(newParentId);
    if (!node || !oldParent || !newParent) {
      return;
    }
    oldParent.children.splice(oldParent.children.indexOf(node), 1);
    newParent.children.splice(index, 0, node);
    this.parents_.set(id, newParent);
    this.requestUpdate();
  }

  private onNodeChanged_(id: string, title: string, url: string) {
    const node = this.nodes_.get(id);
    if (!node) {
      return;
    }
    node.title = title;
    node.url = url;
    this.requestUpdate();
  }

  private onNodeLiveChanged_(id: string, live: boolean) {
    const node = this.nodes_.get(id);
    if (node) {
      node.live = live;
      this.requestUpdate();
    }
  }

  private onNodeRemoved_(id: string) {
    const node = this.nodes_.get(id);
    const parent = this.parents_.get(id);
    if (!node || !parent) {
      return;
    }
    parent.children.splice(parent.children.indexOf(node), 1);
    this.unindex_(node);
    this.requestUpdate();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-app': StrataSidebarAppElement;
  }
}

customElements.define(StrataSidebarAppElement.is, StrataSidebarAppElement);
