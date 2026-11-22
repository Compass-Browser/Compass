// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {SettingsWindowHandlerFactory, SettingsWindowHandlerRemote, SettingsWindowPageCallbackRouter} from '../sidebar.mojom-webui.js';

// Connection to the settings window's browser side (SettingsWindowHandler).
export class SettingsWindowBrowserProxy {
  callbackRouter = new SettingsWindowPageCallbackRouter();
  handler = new SettingsWindowHandlerRemote();

  constructor() {
    SettingsWindowHandlerFactory.getRemote().createHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): SettingsWindowBrowserProxy {
    return instance || (instance = new SettingsWindowBrowserProxy());
  }

  static setInstance(proxy: SettingsWindowBrowserProxy) {
    instance = proxy;
  }
}

let instance: SettingsWindowBrowserProxy|null = null;
