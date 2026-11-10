// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// One live folder refresh, in a dedicated Worker the host page starts for it
// (host.ts). A classic script: it defines globalThis.__compassHost from
// messages to the page, loads the SDK, the prelude and the plugin with
// importScripts(), and posts back what fetchItems() returned.
//
// Nothing here is a security boundary: the plugin shares this global, and the
// browser checks every fetch and the result itself.

'use strict';

(function () {
  const pending = new Map();
  let nextId = 1;
  let started = false;

  function errorOf(e, fallback) {
    return {
      type: 'error',
      code: e && typeof e.code === 'string' ? e.code : fallback,
      message: e && typeof e.message === 'string' ? e.message : String(e),
    };
  }

  function start(message) {
    started = true;
    self.__compassHost = {
      fetch(request) {
        return new Promise((resolve, reject) => {
          const id = nextId++;
          pending.set(id, {resolve, reject});
          self.postMessage({type: 'fetch', id, request});
        });
      },
      log(text) {
        self.postMessage({type: 'log', message: String(text)});
      },
      env: message.env,
    };
    try {
      self.importScripts(...message.scripts);
    } catch (e) {
      self.postMessage(errorOf(e, 'plugin_error'));
      return;
    }
    Promise.resolve()
        .then(() => self.__compassRun(message.config))
        .then((raw) => {
          try {
            self.postMessage({type: 'done', result: raw === undefined ? null : raw});
          } catch (e) {
            self.postMessage({
              type: 'error', code: 'bad_result',
              message: 'fetchItems returned something that is not plain data',
            });
          }
        }, (e) => self.postMessage(errorOf(e, 'plugin_error')));
  }

  self.onmessage = (event) => {
    const message = event.data || {};
    if (message.type === 'start' && !started) {
      start(message);
    } else if (message.type === 'fetchResult') {
      const p = pending.get(message.id);
      if (!p) return;
      pending.delete(message.id);
      if (message.ok) {
        p.resolve(message.response);
      } else {
        const e = new Error(message.message);
        e.code = message.code;
        p.reject(e);
      }
    }
  };
})();
