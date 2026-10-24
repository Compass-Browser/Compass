// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {SidebarBrowserProxy} from './browser_proxy.js';
import './node.js';
import type {Node, Tree} from './sidebar.mojom-webui.js';

// The sidebar: the window's SpaceModel, as a pinned and a Today section. The
// browser sends the tree once, then diffs. Slice b of docs/design/sidebar.md.
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
    };
  }

  protected accessor pinned_: Node|null = null;
  protected accessor today_: Node|null = null;
  protected accessor activeId_: string|null = null;

  private browserProxy_: SidebarBrowserProxy =
      SidebarBrowserProxy.getInstance();
  private listenerIds_: number[] = [];

  // Every node of the tree, by ID, with its parent.
  private nodes_: Map<string, Node> = new Map();
  private parents_: Map<string, Node> = new Map();

  override connectedCallback() {
    super.connectedCallback();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.setTree.addListener(this.onSetTree_.bind(this)),
      router.nodeAdded.addListener(this.onNodeAdded_.bind(this)),
      router.nodeMoved.addListener(this.onNodeMoved_.bind(this)),
      router.nodeChanged.addListener(this.onNodeChanged_.bind(this)),
      router.nodeRemoved.addListener(this.onNodeRemoved_.bind(this)),
      router.activeNodeChanged.addListener(
          (id: string|null) => this.activeId_ = id),
    ];
    // Diffs that arrive before the tree are about nodes the page doesn't know
    // yet, and the lookups below drop them. The tree already has them.
    this.browserProxy_.handler.requestTree();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
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

  protected onNewTabClick_() {
    this.browserProxy_.handler.newTab();
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
