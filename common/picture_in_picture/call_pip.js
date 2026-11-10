// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Picture-in-picture for calls on sites that don't offer their own (Teams,
// Zoom; Meet opens its own document picture-in-picture window). See
// docs/design/picture-in-picture.md, "Calls".
//
// Runs where the page's DOM is shared but its JavaScript isn't: an isolated
// world on desktop (browser/picture_in_picture/call_pip.cc), in each frame
// whose URL is a call's (Zoom's call is in an <iframe>), the CompassPiP
// content world on iOS (ios/App/Browser/AutoPiP.swift; main frame only). On
// a call site it:
// - picks the video to show: the active speaker's tile if the site marks it,
//   else the largest remote one (a <video> playing a MediaStream that isn't
//   the user's own camera, or a canvas the site draws video into, captured),
//   turned as the page turns it on screen (Zoom's portrait phone cameras);
// - registers the Media Session actions picture-in-picture uses while a call
//   is on (the site's hang up button shows): `enterpictureinpicture` (which
//   upstream's auto picture-in-picture
//   calls when the tab or window is left, with a token that allows
//   requestPictureInPicture()), and `togglemicrophone`, `togglecamera` and
//   `hangup`, which press the site's own buttons and show as the window's
//   buttons;
// - follows the speaker: while its video is in picture-in-picture, another
//   tile that stays the pick for kSwapAfter takes its place (no gesture is
//   needed while one is in picture-in-picture);
// - exposes globalThis.compassCallPip { site, inCall(), pick() } for iOS.
//
// A site that registers an action itself after this keeps its own.
(() => {
  if (globalThis.compassCallPip) {
    return;
  }

  // Hosts match as themselves or as a parent domain. Buttons are found by
  // their selectors; what they'd do (on or off) is read from their label,
  // in English and Italian. |speaking|, where a site marks the speaker's
  // tile, wins over the largest tile (which is the speaker's in speaker
  // view). Checked against live calls: see the design doc.
  const SITES = [
    {
      name: 'teams',
      hosts: ['teams.microsoft.com', 'teams.live.com', 'teams.cloud.microsoft'],
      microphone: '#microphone-button',
      camera: '#video-button',
      hangUp: '#hangup-button',
    },
    {
      name: 'zoom',
      hosts: ['zoom.us'],
      // The web client only (/wc/ and app.zoom.us/wc/...).
      path: /^\/wc(\/|$)/,
      // It draws participants into canvases.
      canvas: true,
      microphone: '.join-audio-container__btn',
      camera: '.send-video-container__btn',
      // "End" for the host (it asks whether to end it for everyone).
      hangUp: '.footer__leave-btn, ' +
          ['End', 'Leave', 'Termina', 'Esci', 'Abbandona']
              .map((label) => `button.footer-button__button[aria-label="${label}"]`)
              .join(', '),
    },
  ];

  const matches = (host, pattern) =>
      host === pattern || host.endsWith('.' + pattern);
  const site = SITES.find((s) =>
      s.hosts.some((h) => matches(location.hostname, h)) &&
      (!s.path || s.path.test(location.pathname)));
  if (!site) {
    return;
  }

  const kMinSize = 100;
  const kSwapAfter = 2000;
  const kInterval = 1000;

  const query = (selector) => selector ? document.querySelector(selector) : null;
  const bounds = (element) => element.getBoundingClientRect();
  const area = (element) => {
    const rect = bounds(element);
    return rect.width * rect.height;
  };
  const shown = (element) => {
    const rect = bounds(element);
    return rect.width >= kMinSize && rect.height >= kMinSize &&
        getComputedStyle(element).visibility !== 'hidden';
  };

  // The user's own camera: a track named as one of their cameras (Chrome
  // gives remote tracks a device id too, but names them otherwise, e.g.
  // Teams' "remote video"), or a mirrored preview.
  let cameraLabels = new Set();
  const refreshCameras = () => {
    const devices = navigator.mediaDevices;
    if (!devices || !devices.enumerateDevices) {
      return;
    }
    devices.enumerateDevices().then((list) => {
      cameraLabels = new Set(list.filter((d) => d.kind === 'videoinput' && d.label)
                                 .map((d) => d.label));
    }).catch(() => {});
  };
  const mirrored = (video) =>
      /^matrix\(-1,/.test(getComputedStyle(video).transform);
  const isOwnCamera = (video) => mirrored(video) ||
      video.srcObject.getVideoTracks().some((track) =>
          cameraLabels.has(track.label));
  const isRemoteVideo = (video) =>
      video instanceof HTMLVideoElement &&
      video.srcObject instanceof MediaStream &&
      video.srcObject.getVideoTracks().some((t) => t.readyState === 'live') &&
      !isOwnCamera(video);

  // How far the page turns |element| on screen (its CSS transform and its
  // ancestors'), in clockwise quarter turns as degrees: Zoom shows a phone's
  // portrait camera by turning the tile, so its frames come sideways.
  const turnOf = (element) => {
    if (typeof DOMMatrix !== 'function') {
      return 0;
    }
    let matrix = new DOMMatrix();
    for (let e = element; e && e !== document.documentElement;
         e = e.parentElement) {
      const transform = getComputedStyle(e).transform;
      if (transform && transform !== 'none') {
        matrix = new DOMMatrix(transform).multiply(matrix);
      }
    }
    // A mirror isn't a turn (and is left as it is).
    if (matrix.a * matrix.d - matrix.b * matrix.c <= 0) {
      return 0;
    }
    const degrees = Math.round(
        Math.atan2(matrix.b, matrix.a) * 180 / Math.PI / 90) * 90;
    return ((degrees % 360) + 360) % 360;
  };

  const hiddenVideo = (stream) => {
    const video = document.createElement('video');
    video.muted = true;
    video.playsInline = true;
    video.setAttribute('aria-hidden', 'true');
    video.style.cssText = 'position:fixed;right:0;bottom:0;width:1px;' +
        'height:1px;opacity:0;pointer-events:none;';
    video.srcObject = stream;
    document.documentElement.append(video);
    video.play().catch(() => {});
    return video;
  };

  // |track|'s frames turned by |turn| degrees (the frames say so; nothing
  // is drawn), or null where the browser can't (WebKit).
  const turnedStream = (track, turn) => {
    if (typeof MediaStreamTrackProcessor !== 'function' ||
        typeof MediaStreamTrackGenerator !== 'function' ||
        typeof VideoFrame !== 'function') {
      return null;
    }
    const processor = new MediaStreamTrackProcessor({track});
    const generator = new MediaStreamTrackGenerator({kind: 'video'});
    processor.readable
        .pipeThrough(new TransformStream({
          transform(frame, controller) {
            let turned = null;
            try {
              turned = new VideoFrame(frame, {rotation: turn});
            } catch (e) {
              // Kept as it comes.
            }
            frame.close();
            if (turned) {
              controller.enqueue(turned);
            }
          },
        }))
        .pipeTo(generator.writable)
        .catch(() => {});
    return new MediaStream([generator]);
  };

  // A video for picture-in-picture to show |element| as the page does: a
  // canvas the site draws into, captured, or a tile the page turns, turned
  // the same. Made ahead (update()), as the video needs a frame before it
  // can go into the window. Null for a <video> shown as it is (or that
  // can't be turned): it goes in itself.
  const madeVideos = new WeakMap();
  const madeVideo = (element, turn) => {
    const canvas = element instanceof HTMLCanvasElement;
    if (!canvas && !turn) {
      return null;
    }
    const made = madeVideos.get(element);
    if (made && made.turn === turn &&
        (!made.video || made.video.isConnected)) {
      return made.video;
    }
    if (made && made.video &&
        made.video !== document.pictureInPictureElement) {
      made.track.stop();
      made.video.remove();
    }
    let video = null;
    let track = null;
    try {
      track = canvas ? element.captureStream(30).getVideoTracks()[0] :
                       element.srcObject.getVideoTracks()[0].clone();
      let stream = turn ? turnedStream(track, turn) : null;
      if (!stream && canvas) {
        stream = new MediaStream([track]);
      }
      if (stream) {
        video = hiddenVideo(stream);
      } else {
        track.stop();
      }
    } catch (e) {
      // Not one to make (a tile gone meanwhile): tried again next time.
      return null;
    }
    madeVideos.set(element, {turn, track, video});
    return video;
  };

  const candidates = () => {
    const found = [...document.querySelectorAll('video')].filter(
        (v) => isRemoteVideo(v) && shown(v) && v.readyState >= 2);
    if (site.canvas) {
      for (const canvas of document.querySelectorAll('canvas')) {
        if (shown(canvas)) {
          found.push(canvas);
        }
      }
    }
    return found;
  };

  // The element to show: the speaker's tile if marked, else the largest.
  const pickElement = () => {
    const all = candidates();
    if (!all.length) {
      return null;
    }
    // The speaker's tile: the closest of its ancestors that holds one.
    for (let tile = query(site.speaking), up = 0; tile && up < 6;
         tile = tile.parentElement, up++) {
      const inside = all.filter((e) => tile.contains(e));
      if (inside.length) {
        return inside.reduce((a, b) => area(b) > area(a) ? b : a);
      }
    }
    return all.reduce((a, b) => area(b) > area(a) ? b : a);
  };
  // Sites switch picture-in-picture off on their tiles (Teams sets
  // disablePictureInPicture): on a call, the pick gets it back, as the
  // window is what Compass offers here.
  const pick = () => {
    const element = pickElement();
    if (!element) {
      return null;
    }
    const video = madeVideo(element, turnOf(element)) ||
        (element instanceof HTMLVideoElement ? element : null);
    if (video && video.disablePictureInPicture) {
      video.disablePictureInPicture = false;
    }
    return video;
  };

  // On while the site shows its hang up button: tiles come and go (Zoom
  // drops them while its page is hidden), and the handlers have to be there
  // when the tab is left.
  const inCall = () => !!query(site.hangUp);

  const labelOf = (button) =>
      (button.getAttribute('aria-label') || button.title ||
       button.textContent || '').trim().toLowerCase();
  // Whether pressing it would turn something on (so it's off now): "Unmute",
  // "Turn camera on", "Start video", "Riattiva", "Attiva", "Avvia".
  const turnsOn = (button) =>
      /\b(unmute|start|on)\b|^(riattiva|attiva|avvia)\b/.test(labelOf(button));
  const press = (selector) => {
    const button = query(selector);
    if (button) {
      button.click();
    }
  };

  // Media Session. WebKit lacks most of these actions: each is tried alone.
  // Not touched before a call is on in this frame: a frame using it may get
  // the tab's actions routed to it (Zoom's outer page, around the call's
  // frame).
  const registered = new Set();
  const setHandler = (action, handler) => {
    if (!handler && !registered.has(action)) {
      return;
    }
    const session = navigator.mediaSession;
    if (!session) {
      return;
    }
    try {
      session.setActionHandler(action, handler);
      if (handler) {
        registered.add(action);
      } else {
        registered.delete(action);
      }
    } catch (e) {
      // Not an action this browser knows.
    }
  };

  // The video Compass put in the window, and the one that's been the pick
  // since |pickSince|.
  let shownVideo = null;
  let nextVideo = null;
  let pickSince = 0;

  const enter = async (video) => {
    if (!video) {
      return;
    }
    try {
      await video.requestPictureInPicture();
      shownVideo = video;
    } catch (e) {
      // Not ready yet (a canvas without a frame), or not allowed.
    }
  };

  const handlers = {
    enterpictureinpicture: () => enter(pick()),
    togglemicrophone: () => press(site.microphone),
    togglecamera: () => press(site.camera),
    hangup: () => press(site.hangUp),
  };

  const update = () => {
    // Labels show once the page may use the camera: asked again each time.
    refreshCameras();
    const on = inCall();
    for (const [action, handler] of Object.entries(handlers)) {
      setHandler(action, on ? handler : null);
    }
    if (!on) {
      return;
    }
    const session = navigator.mediaSession;
    if (session && session.setMicrophoneActive) {
      const microphone = query(site.microphone);
      if (microphone) {
        session.setMicrophoneActive(!turnsOn(microphone));
      }
    }
    if (session && session.setCameraActive) {
      const camera = query(site.camera);
      if (camera) {
        session.setCameraActive(!turnsOn(camera));
      }
    }
    const picked = pick();
    // Follow the speaker while in the window.
    if (shownVideo && document.pictureInPictureElement === shownVideo &&
        picked && picked !== shownVideo) {
      if (picked !== nextVideo) {
        nextVideo = picked;
        pickSince = Date.now();
      } else if (Date.now() - pickSince >= kSwapAfter) {
        nextVideo = null;
        enter(picked);
      }
    } else {
      nextVideo = null;
    }
  };

  document.addEventListener('leavepictureinpicture', (event) => {
    if (event.target === shownVideo) {
      shownVideo = null;
    }
  }, true);

  setInterval(update, kInterval);
  update();

  globalThis.compassCallPip = {site: site.name, inCall, pick};
})();
