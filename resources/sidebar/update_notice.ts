// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from './browser_proxy.js';
import {UpdateNoticeAction} from './sidebar.mojom-webui.js';
import {getCss} from './update_notice.css.js';
import {getHtml} from './update_notice.html.js';

// The "update ready" card at the bottom of the sidebar, above the now
// playing widgets and drawn like them (docs/design/sidebar.md, "Update
// ready"): an update is downloaded and a relaunch installs it. The browser
// decides when it shows (the About page's switch, closing it).
export class CompassUpdateNoticeElement extends CrLitElement {
  static get is() {
    return 'compass-update-notice';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      ready: {type: Boolean, reflect: true},
    };
  }

  accessor ready: boolean = false;

  protected onRelaunchClick_(e: Event) {
    e.stopPropagation();
    SidebarBrowserProxy.getInstance().handler.actOnUpdateNotice(
        UpdateNoticeAction.kRelaunch);
  }

  protected onCloseClick_(e: Event) {
    e.stopPropagation();
    SidebarBrowserProxy.getInstance().handler.actOnUpdateNotice(
        UpdateNoticeAction.kDismiss);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-update-notice': CompassUpdateNoticeElement;
  }
}

customElements.define(CompassUpdateNoticeElement.is, CompassUpdateNoticeElement);
