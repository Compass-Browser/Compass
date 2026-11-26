// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../space_editor.mojom-webui.js';

// Connection to the browser-side SpaceEditorPageHandler
// (space_editor_ui.cc).
export class SpaceEditorBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): SpaceEditorBrowserProxy {
    return instance || (instance = new SpaceEditorBrowserProxy());
  }
}

let instance: SpaceEditorBrowserProxy|null = null;
