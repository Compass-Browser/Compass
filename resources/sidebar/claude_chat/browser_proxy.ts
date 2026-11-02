// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../chat.mojom-webui.js';

// Connection to the browser-side ClaudeChatPageHandler.
export class ClaudeChatBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): ClaudeChatBrowserProxy {
    return instance || (instance = new ClaudeChatBrowserProxy());
  }

  static setInstance(proxy: ClaudeChatBrowserProxy) {
    instance = proxy;
  }
}

let instance: ClaudeChatBrowserProxy|null = null;
