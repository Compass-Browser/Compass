// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {SidebarBrowserProxy} from './browser_proxy.js';
import type {Tab} from './sidebar.mojom-webui.js';

// The sidebar: the window's tabs, top to bottom. Slice a of
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
      tabs_: {type: Array},
    };
  }

  protected accessor tabs_: Tab[] = [];

  private browserProxy_: SidebarBrowserProxy =
      SidebarBrowserProxy.getInstance();
  private tabsChangedListenerId_: number|null = null;

  override connectedCallback() {
    super.connectedCallback();
    this.tabsChangedListenerId_ =
        this.browserProxy_.callbackRouter.tabsChanged.addListener(
            (tabs: Tab[]) => this.tabs_ = tabs);
    this.browserProxy_.handler.getTabs().then(({tabs}) => this.tabs_ = tabs);
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.tabsChangedListenerId_ !== null) {
      this.browserProxy_.callbackRouter.removeListener(
          this.tabsChangedListenerId_);
      this.tabsChangedListenerId_ = null;
    }
  }

  protected getTitle_(tab: Tab): string {
    return tab.title || tab.url;
  }

  protected getFaviconUrl_(tab: Tab): string {
    const params = new URLSearchParams({
      size: '16',
      scaleFactor: `${window.devicePixelRatio}x`,
      pageUrl: tab.url,
      allowGoogleServerFallback: '0',
    });
    return `chrome://favicon2/?${params.toString()}`;
  }

  protected onTabClick_(e: Event) {
    this.browserProxy_.handler.activateTab(this.getTabId_(e));
  }

  protected onCloseClick_(e: Event) {
    e.stopPropagation();
    this.browserProxy_.handler.closeTab(this.getTabId_(e));
  }

  protected onNewTabClick_() {
    this.browserProxy_.handler.newTab();
  }

  private getTabId_(e: Event): number {
    return Number((e.currentTarget as HTMLElement).dataset['tabId']);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'strata-sidebar-app': StrataSidebarAppElement;
  }
}

customElements.define(StrataSidebarAppElement.is, StrataSidebarAppElement);
