// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// In place of Google's translate library on the desktop
// (docs/design/translation.md, "The script"): upstream's translate.js,
// which Chromium runs in a page's translate world, drives
// google.translate.TranslateService; this one translates with
// page_translator.js and compassTranslate.send(), the browser's on-device
// engine (renderer/translate/page_translator_bindings.cc). Loaded after
// page_translator.js.

(() => {
  'use strict';
  // translate.js's codes for a failure (te.dom.DomTranslator.Error).
  const TRANSLATION_ERROR = 1;
  const UNSUPPORTED_LANGUAGE = 2;

  // compassTranslate.send() (the renderer's binding) as a promise.
  const translate = (source, target, segments) => new Promise(
      (resolve, reject) => compassTranslate.send(
          source, target, segments,
          (result, translations) => result === 'ok' ?
              resolve(translations) :
              reject(new Error(result))));

  const service = () => {
    let detected = 'und';
    return {
      isAvailable() {
        return typeof compassTranslate === 'object' &&
            typeof __compassTranslator === 'object';
      },

      translatePage(source, target, progress) {
        if (!source || source === 'auto' || source === 'und') {
          progress(0, true, UNSUPPORTED_LANGUAGE);
          return;
        }
        detected = source;
        let error = 0;
        const engine = async segments => {
          try {
            return await translate(source, target, segments);
          } catch (e) {
            error = e?.message === 'unsupported' ? UNSUPPORTED_LANGUAGE :
                                                   TRANSLATION_ERROR;
            throw e;
          }
        };
        __compassTranslator.start({source, target, engine});
        progress(0, false);
        __compassTranslator.idle().then(() => {
          const {done, failed} = __compassTranslator.progress();
          if (error && !done) {
            progress(100, true, error);
          } else {
            progress(100, true);
          }
        });
      },

      restore() {
        __compassTranslator.restore();
      },

      getDetectedLanguage() {
        return detected;
      },
    };
  };

  globalThis.google ??= {};
  google.translate ??= {};
  google.translate.TranslateService = service;
  cr.googleTranslate.onTranslateElementLoad();
})();
