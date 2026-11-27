// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../archive.mojom-webui.js';

// Connection to the browser-side ArchivePageHandler
// (archive_ui.cc).
export class ArchiveBrowserProxy {
  callbackRouter: PageCallbackRouter = new PageCallbackRouter();
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.callbackRouter.$.bindNewPipeAndPassRemote(),
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): ArchiveBrowserProxy {
    return instance || (instance = new ArchiveBrowserProxy());
  }
}

let instance: ArchiveBrowserProxy|null = null;
