// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from './easel.mojom-webui.js';

// Connection to the browser-side compass::easel::EaselPageHandler.
export class EaselBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): EaselBrowserProxy {
    return instance || (instance = new EaselBrowserProxy());
  }

  static setInstance(proxy: EaselBrowserProxy) {
    instance = proxy;
  }
}

let instance: EaselBrowserProxy|null = null;
