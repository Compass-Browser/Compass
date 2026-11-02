// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../find_bar.mojom-webui.js';

// Connection to the browser-side compass::FindBar.
export class FindBarBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): FindBarBrowserProxy {
    return instance || (instance = new FindBarBrowserProxy());
  }

  static setInstance(proxy: FindBarBrowserProxy) {
    instance = proxy;
  }
}

let instance: FindBarBrowserProxy|null = null;
