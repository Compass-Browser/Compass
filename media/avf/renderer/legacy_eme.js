// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Safari's legacy, prefixed EME (WebKitMediaKeys) on top of the standard
// com.apple.fps EME (docs/design/fairplay.md "Legacy API"). Runs in the main
// world at document start, only in pages presenting as Safari: players that
// find window.WebKitMediaKeys (Bitmovin, older Brightcove and JW Player
// builds, Apple's FPS sample) take this path and never the standard one.
//
// The legacy flow, as WebKit's CDMSessionAVFoundationObjC:
//   <video> "webkitneedkey"  initData = [u32 LE length][skd:// URI, UTF-16]
//   new WebKitMediaKeys('com.apple.fps.1_0'); video.webkitSetMediaKeys(keys)
//   keys.createSession('video/mp4', initData + [u32][content id][u32][cert])
//   session "webkitkeymessage" (the SPC) -> session.update(ckc)
//   session "webkitkeyadded" or "webkitkeyerror"
(() => {
  'use strict';
  if ('WebKitMediaKeys' in window || !navigator.requestMediaKeySystemAccess) {
    return;
  }

  // The page's own scripts may replace these later; keep the originals.
  const requestAccess = Navigator.prototype.requestMediaKeySystemAccess;
  const setMediaKeys = HTMLMediaElement.prototype.setMediaKeys;
  const addListener = EventTarget.prototype.addEventListener;
  const defineProperty = Object.defineProperty;

  const CONFIG = [{
    initDataTypes: ['skd'],
    videoCapabilities: [{contentType: 'application/vnd.apple.mpegurl'}],
    audioCapabilities: [{contentType: 'application/vnd.apple.mpegurl'}],
  }];
  const TYPES = new Set(['', 'video/mp4', 'audio/mp4', 'application/x-mpegurl',
                         'application/vnd.apple.mpegurl']);

  const isKeySystem = (ks) =>
      typeof ks === 'string' && (ks === 'com.apple.fps' ||
                                 ks.startsWith('com.apple.fps.'));
  const domError = (name, message) => new DOMException(message, name);

  // Attributes Safari has, as on* properties that hold one listener.
  function eventHandlerProperty(proto, type) {
    const key = Symbol(type);
    const installed = Symbol(type + ' listener');
    defineProperty(proto, 'on' + type, {
      configurable: true,
      enumerable: true,
      get() { return this[key] || null; },
      set(handler) {
        if (!this[installed]) {
          this[installed] = true;
          addListener.call(this, type, (e) => {
            if (typeof this[key] === 'function') this[key].call(this, e);
          });
        }
        this[key] = typeof handler === 'function' ? handler : null;
      },
    });
  }

  function event(type, props) {
    const e = new Event(type);
    for (const [name, value] of Object.entries(props)) {
      defineProperty(e, name, {value, enumerable: true});
    }
    return e;
  }

  function bytesOf(data) {
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    return null;
  }

  // [u32 LE length][UTF-16 skd:// URI], as webkitneedkey's initData.
  function needKeyInitData(uri) {
    const out = new Uint8Array(4 + uri.length * 2);
    const view = new DataView(out.buffer);
    view.setUint32(0, uri.length * 2, true);
    for (let i = 0; i < uri.length; ++i) {
      view.setUint16(4 + i * 2, uri.charCodeAt(i), true);
    }
    return out;
  }

  // createSession()'s initData: the needkey initData, then the content id and
  // the application certificate, each after its u32 LE length. Pages that
  // pass just the needkey initData set the certificate some other way (they
  // can't in Safari, so none do); that one's an error.
  function parseSessionInitData(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const parts = [];
    let offset = 0;
    while (offset + 4 <= bytes.byteLength && parts.length < 3) {
      const length = view.getUint32(offset, true);
      offset += 4;
      if (offset + length > bytes.byteLength) return null;
      parts.push(bytes.subarray(offset, offset + length));
      offset += length;
    }
    if (parts.length !== 3 || parts[0].byteLength % 2) return null;
    const uriView = new DataView(parts[0].buffer, parts[0].byteOffset,
                                 parts[0].byteLength);
    let uri = '';
    for (let i = 0; i < parts[0].byteLength; i += 2) {
      uri += String.fromCharCode(uriView.getUint16(i, true));
    }
    if (!uri.startsWith('skd://') || !parts[2].byteLength) return null;
    return {uri, certificate: parts[2].slice()};
  }

  class WebKitMediaKeyError {
    constructor(code, systemCode) {
      defineProperty(this, 'code', {value: code, enumerable: true});
      defineProperty(this, 'systemCode', {value: systemCode, enumerable: true});
    }
  }
  const ERRORS = {MEDIA_KEYERR_UNKNOWN: 1, MEDIA_KEYERR_CLIENT: 2,
                  MEDIA_KEYERR_SERVICE: 3, MEDIA_KEYERR_OUTPUT: 4,
                  MEDIA_KEYERR_HARDWARECHANGE: 5, MEDIA_KEYERR_DOMAIN: 6};
  for (const [name, value] of Object.entries(ERRORS)) {
    defineProperty(WebKitMediaKeyError, name, {value, enumerable: true});
    defineProperty(WebKitMediaKeyError.prototype, name,
                   {value, enumerable: true});
  }

  const internals = new WeakMap();

  class WebKitMediaKeySession extends EventTarget {
    constructor() {
      throw new TypeError('Illegal constructor');
    }
    get keySystem() { return internals.get(this).keySystem; }
    get sessionId() { return internals.get(this).sessionId; }
    get error() { return internals.get(this).error; }

    update(key) {
      const self = internals.get(this);
      const bytes = bytesOf(key);
      if (!bytes || !bytes.byteLength) {
        throw domError('InvalidAccessError', 'The key is empty.');
      }
      const ckc = bytes.slice();
      self.ready
          .then((session) => session && session.update(ckc))
          .catch(() => self.fail(ERRORS.MEDIA_KEYERR_CLIENT));
    }

    close() {
      const self = internals.get(this);
      self.closed = true;
      self.ready.then((session) => session && session.close()).catch(() => {});
    }
  }
  for (const type of ['webkitkeymessage', 'webkitkeyadded', 'webkitkeyerror']) {
    eventHandlerProperty(WebKitMediaKeySession.prototype, type);
  }

  function createSession(keys, initData) {
    const parsed = parseSessionInitData(initData);
    const target = new EventTarget();
    Object.setPrototypeOf(target, WebKitMediaKeySession.prototype);
    const self = {keySystem: keys.keySystem, sessionId: '', error: null,
                  closed: false};
    self.fail = (code) => {
      if (self.closed) return;
      self.error = new WebKitMediaKeyError(code, 0);
      target.dispatchEvent(event('webkitkeyerror', {}));
    };
    internals.set(target, self);

    self.ready = (async () => {
      if (!parsed) {
        throw domError('InvalidAccessError', 'Unexpected initData.');
      }
      const mediaKeys = await internals.get(keys).mediaKeys;
      await mediaKeys.setServerCertificate(parsed.certificate);
      const session = mediaKeys.createSession('temporary');
      addListener.call(session, 'message', (e) => {
        if (self.closed) return;
        target.dispatchEvent(event('webkitkeymessage', {
          message: new Uint8Array(e.message),
          destinationURL: '',
        }));
      });
      addListener.call(session, 'keystatuseschange', () => {
        if (self.closed) return;
        for (const [, status] of session.keyStatuses) {
          if (status === 'usable') {
            target.dispatchEvent(event('webkitkeyadded', {}));
          } else if (status === 'internal-error') {
            self.fail(ERRORS.MEDIA_KEYERR_UNKNOWN);
          } else if (status === 'output-restricted' ||
                     status === 'output-downscaled') {
            self.fail(ERRORS.MEDIA_KEYERR_OUTPUT);
          }
        }
      });
      await session.generateRequest('skd',
                                    new TextEncoder().encode(parsed.uri));
      self.sessionId = session.sessionId;
      return session;
    })();
    self.ready.catch(() => {
      // As Safari, asynchronously, after createSession() returned.
      self.fail(ERRORS.MEDIA_KEYERR_CLIENT);
    });
    return target;
  }

  class WebKitMediaKeys {
    constructor(keySystem) {
      if (!isKeySystem(keySystem)) {
        throw domError('NotSupportedError',
                       'Unsupported key system: ' + keySystem);
      }
      const mediaKeys = requestAccess.call(navigator, 'com.apple.fps', CONFIG)
                            .then((access) => access.createMediaKeys());
      mediaKeys.catch(() => {});
      internals.set(this, {keySystem, mediaKeys});
    }
    get keySystem() { return internals.get(this).keySystem; }

    createSession(type, initData) {
      if (typeof type === 'string' && !TYPES.has(type.toLowerCase())) {
        throw domError('NotSupportedError', 'Unsupported type: ' + type);
      }
      const bytes = bytesOf(initData);
      if (!bytes || !bytes.byteLength) {
        throw domError('InvalidAccessError', 'The initData is empty.');
      }
      return createSession(this, bytes.slice());
    }

    static isTypeSupported(keySystem, type) {
      return isKeySystem(keySystem) &&
             (type === undefined || type === null ||
              TYPES.has(String(type).split(';')[0].trim().toLowerCase()));
    }
  }

  const elementKeys = new WeakMap();
  defineProperty(HTMLMediaElement.prototype, 'webkitKeys', {
    configurable: true,
    enumerable: true,
    get() { return elementKeys.get(this) || null; },
  });
  defineProperty(HTMLMediaElement.prototype, 'webkitSetMediaKeys', {
    configurable: true,
    writable: true,
    value: function webkitSetMediaKeys(keys) {
      if (keys !== null && !(keys instanceof WebKitMediaKeys)) {
        throw new TypeError('Not a WebKitMediaKeys.');
      }
      if (keys) {
        elementKeys.set(this, keys);
      } else {
        elementKeys.delete(this);
      }
      const media = keys ? internals.get(keys).mediaKeys : Promise.resolve(null);
      media.then((mediaKeys) => setMediaKeys.call(this, mediaKeys))
          .catch(() => {});
    },
  });
  eventHandlerProperty(HTMLMediaElement.prototype, 'webkitneedkey');

  // AVPlayer's key requests reach the page as "encrypted" (initDataType skd,
  // the URI in UTF-8); Safari also fires "webkitneedkey" for them. Capturing
  // at the window sees media elements in the document.
  addListener.call(window, 'encrypted', (e) => {
    if (e.initDataType !== 'skd' || !(e.target instanceof HTMLMediaElement)) {
      return;
    }
    const uri = new TextDecoder().decode(e.initData);
    e.target.dispatchEvent(
        event('webkitneedkey', {initData: needKeyInitData(uri)}));
  }, true);

  for (const [name, value] of Object.entries({
         WebKitMediaKeys, WebKitMediaKeySession, WebKitMediaKeyError})) {
    defineProperty(window, name, {configurable: true, writable: true, value});
  }
})();
