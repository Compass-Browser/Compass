// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from './sidebar.mojom-webui.js';

// Connection to the browser-side SidebarPageHandler.
export class SidebarBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): SidebarBrowserProxy {
    return instance || (instance = new SidebarBrowserProxy());
  }

  static setInstance(proxy: SidebarBrowserProxy) {
    instance = proxy;
  }
}

let instance: SidebarBrowserProxy|null = null;
