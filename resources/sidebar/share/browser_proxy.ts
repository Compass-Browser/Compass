// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../share.mojom-webui.js';

// Connection to the browser-side SharePageHandler (compass_share_ui.cc).
export class ShareBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): ShareBrowserProxy {
    return instance || (instance = new ShareBrowserProxy());
  }
}

let instance: ShareBrowserProxy|null = null;
