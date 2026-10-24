// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../command_bar.mojom-webui.js';

// Connection to the browser-side CommandBarPageHandler.
export class CommandBarBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): CommandBarBrowserProxy {
    return instance || (instance = new CommandBarBrowserProxy());
  }

  static setInstance(proxy: CommandBarBrowserProxy) {
    instance = proxy;
  }
}

let instance: CommandBarBrowserProxy|null = null;
