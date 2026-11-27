// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PageHandlerFactory, PageHandlerRemote} from '../live_folder_editor.mojom-webui.js';

// Connection to the browser-side LiveFolderEditorPageHandler
// (live_folder_editor_ui.cc).
export class LiveFolderEditorBrowserProxy {
  handler: PageHandlerRemote = new PageHandlerRemote();

  constructor() {
    PageHandlerFactory.getRemote().createPageHandler(
        this.handler.$.bindNewPipeAndPassReceiver());
  }

  static getInstance(): LiveFolderEditorBrowserProxy {
    return instance || (instance = new LiveFolderEditorBrowserProxy());
  }
}

let instance: LiveFolderEditorBrowserProxy|null = null;
