// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Capture to Easel's side in the page (docs/design/easels.md, "Captures"):
// the overlay to drag a region over and pick a board, the confirmation, and
// what Refresh needs (where the page is scrolled). Capture Screenshot uses
// the same overlay (docs/design/screenshots.md): the picker offers Copy and
// Save instead of boards, and Full Page takes the whole page. Runs in the isolated world
// of the main frame (easel_capture_core.cc), so the page's scripts can't see
// these functions or the overlay's closed shadow root. The browser waits for
// the user by calling poll() until it returns something.
window.__compassEasel = window.__compassEasel || (() => {
  'use strict';

  // Smaller drags are taken as a click: the overlay stays for another try.
  const MIN_SIZE = 8;
  const TOAST_MS = 2400;

  let session = null;
  let result = null;
  // A pick waiting for the overlay to leave the page's frames.
  let pending = false;
  let toastHost = null;
  let toastTimer = 0;

  function view() {
    const vv = window.visualViewport;
    return {
      scrollX: window.scrollX,
      scrollY: window.scrollY,
      innerWidth: window.innerWidth,
      innerHeight: window.innerHeight,
      devicePixelRatio: window.devicePixelRatio || 1,
      visualLeft: vv ? vv.offsetLeft : 0,
      visualTop: vv ? vv.offsetTop : 0,
      visualScale: vv ? vv.scale : 1,
    };
  }

  // A host in the top layer (above the page's own dialogs and fullscreen
  // elements where popovers are supported) with a closed shadow root.
  function makeHost(css) {
    const host = document.createElement('div');
    host.setAttribute('popover', 'manual');
    host.style.cssText =
        'all:initial;position:fixed;inset:0;margin:0;padding:0;border:0;' +
        'width:100vw;height:100vh;max-width:none;max-height:none;' +
        'background:transparent;overflow:visible;z-index:2147483647;' +
        'pointer-events:none;';
    const root = host.attachShadow({mode: 'closed'});
    const style = document.createElement('style');
    style.textContent = css;
    root.appendChild(style);
    (document.documentElement || document).appendChild(host);
    try {
      host.showPopover();
    } catch (e) {
      // No popover support, or the document can't show one: z-index it is.
    }
    return {host, root};
  }

  const OVERLAY_CSS = `
    :host { color-scheme: light dark; }
    * { box-sizing: border-box; }
    .stage {
      position: fixed; inset: 0; cursor: crosshair; pointer-events: auto;
      background: rgba(0, 0, 0, 0.35); touch-action: none;
      font: 13px/1.35 system-ui, -apple-system, "Segoe UI", sans-serif;
    }
    .stage[selecting] { background: transparent; }
    .sel {
      position: fixed; display: none; border-radius: 2px;
      outline: 2px solid #fff; outline-offset: 0;
      box-shadow: 0 0 0 100vmax rgba(0, 0, 0, 0.35);
    }
    .stage[selecting] .sel { display: block; }
    .hint {
      position: fixed; top: 16px; left: 50%; transform: translateX(-50%);
      display: flex; align-items: center; gap: 10px;
      padding: 7px 14px; border-radius: 999px; pointer-events: none;
      background: rgba(32, 33, 36, 0.92); color: #fff; white-space: nowrap;
      box-shadow: 0 2px 10px rgba(0, 0, 0, 0.25);
    }
    .hint button.full {
      display: inline-block; width: auto; margin: -3px -8px -3px 0;
      padding: 3px 10px; border-radius: 999px; pointer-events: auto;
      background: rgba(255, 255, 255, 0.16); color: #fff; font-weight: 600;
    }
    .hint button.full:hover, .hint button.full:focus-visible {
      background: rgba(255, 255, 255, 0.28);
    }
    .stage[picking] .hint { display: none; }
    .picker {
      position: fixed; display: none; width: 240px; padding: 6px;
      border-radius: 12px; cursor: default; pointer-events: auto;
      background: Canvas; color: CanvasText;
      box-shadow: 0 8px 28px rgba(0, 0, 0, 0.3);
    }
    .stage[picking] .picker { display: block; }
    .heading {
      padding: 6px 8px 4px; font-size: 11px; font-weight: 600;
      text-transform: uppercase; letter-spacing: 0.04em; opacity: 0.6;
    }
    .list { max-height: 248px; overflow-y: auto; }
    button {
      display: block; width: 100%; margin: 0; padding: 7px 8px; border: 0;
      border-radius: 7px; background: transparent; color: inherit;
      font: inherit; text-align: start; cursor: pointer;
      overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
    }
    button:hover, button:focus-visible {
      background: rgba(127, 127, 127, 0.18); outline: none;
    }
    .new { font-weight: 600; }
    .sep { height: 1px; margin: 4px 2px; background: rgba(127, 127, 127, 0.3); }
    .cancel { opacity: 0.7; }
  `;

  const TOAST_CSS = `
    .toast {
      position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%);
      max-width: min(480px, calc(100vw - 32px)); padding: 9px 16px;
      border-radius: 999px; background: rgba(32, 33, 36, 0.94); color: #fff;
      font: 13px/1.35 system-ui, -apple-system, "Segoe UI", sans-serif;
      overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
      box-shadow: 0 4px 16px rgba(0, 0, 0, 0.3);
    }
  `;

  function el(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function removeOverlay() {
    if (!session) return;
    window.removeEventListener('keydown', session.onKey, true);
    session.host.remove();
    session = null;
  }

  function finish(value) {
    removeOverlay();
    if (value.state !== 'picked') {
      result = value;
      return;
    }
    // The browser copies the page's next frames: wait until the overlay is
    // gone from them.
    pending = true;
    requestAnimationFrame(() => requestAnimationFrame(() => {
      pending = false;
      result = value;
    }));
  }

  // |boards|: Capture to Easel's picker. |options.screenshot|: Capture
  // Screenshot's instead (Copy, Save) and a Full Page button.
  function start(boards, options) {
    const screenshot = !!(options && options.screenshot);
    removeOverlay();
    result = null;
    pending = false;
    const {host, root} = makeHost(OVERLAY_CSS);
    const stage = el('div', 'stage');
    stage.tabIndex = -1;
    const sel = el('div', 'sel');
    const hint = el('div', 'hint');
    hint.appendChild(el('span', '', 'Drag over what to capture · Esc to cancel'));
    const picker = el('div', 'picker');
    stage.append(sel, hint, picker);
    root.appendChild(stage);

    const s = {host, stage, sel, picker, rect: null, from: null, onKey: null,
               full: false};
    session = s;

    s.onKey = (e) => {
      if (session !== s || !e.isTrusted || e.key !== 'Escape') return;
      e.preventDefault();
      e.stopImmediatePropagation();
      finish({state: 'cancelled'});
    };
    window.addEventListener('keydown', s.onKey, true);
    // The page under the overlay doesn't scroll while a region is picked.
    stage.addEventListener('wheel', (e) => e.preventDefault(), {passive: false});

    function place(x1, y1, x2, y2) {
      const left = Math.max(0, Math.min(x1, x2));
      const top = Math.max(0, Math.min(y1, y2));
      const right = Math.min(window.innerWidth, Math.max(x1, x2));
      const bottom = Math.min(window.innerHeight, Math.max(y1, y2));
      s.rect = {x: left, y: top, width: Math.max(0, right - left),
                height: Math.max(0, bottom - top)};
      sel.style.left = `${s.rect.x}px`;
      sel.style.top = `${s.rect.y}px`;
      sel.style.width = `${s.rect.width}px`;
      sel.style.height = `${s.rect.height}px`;
    }

    function pick(easel) {
      if (session !== s || !s.rect) return;
      finish(Object.assign({state: 'picked', easel}, s.rect, view()));
    }

    // Capture Screenshot: |action| is 'copy' or 'save'.
    function pickAction(action) {
      if (session !== s) return;
      if (s.full) {
        finish(Object.assign({state: 'picked', action, full: true}, view()));
        return;
      }
      if (!s.rect) return;
      finish(Object.assign({state: 'picked', action}, s.rect, view()));
    }

    function screenshotList() {
      const list = el('div', 'list');
      const copy = el('button', 'new', 'Copy');
      copy.addEventListener('click', (e) => {
        if (e.isTrusted) pickAction('copy');
      });
      const save = el('button', '', 'Save to Downloads');
      save.addEventListener('click', (e) => {
        if (e.isTrusted) pickAction('save');
      });
      list.append(copy, save);
      return list;
    }

    if (screenshot) {
      const full = el('button', 'full', 'Full Page');
      full.addEventListener('click', (e) => {
        if (!e.isTrusted || session !== s) return;
        e.stopPropagation();
        s.full = true;
        s.rect = null;
        stage.removeAttribute('selecting');
        showPicker();
      });
      full.addEventListener('pointerdown', (e) => e.stopPropagation());
      hint.appendChild(full);
    }

    function showPicker() {
      picker.textContent = '';
      picker.appendChild(el('div', 'heading',
                            screenshot ? (s.full ? 'Full Page' : 'Screenshot') :
                                         'Add to Easel'));
      const list = screenshot ? screenshotList() : el('div', 'list');
      for (const board of screenshot ? [] : boards) {
        const button = el('button', '', board.title || 'Untitled Easel');
        button.title = board.title || 'Untitled Easel';
        button.addEventListener('click', (e) => {
          if (e.isTrusted) pick(board.id);
        });
        list.appendChild(button);
      }
      if (!screenshot) {
        const fresh = el('button', 'new', 'New Easel');
        fresh.addEventListener('click', (e) => {
          if (e.isTrusted) pick('');
        });
        list.appendChild(fresh);
      }
      picker.appendChild(list);
      picker.appendChild(el('div', 'sep'));
      const cancel = el('button', 'cancel', 'Cancel');
      cancel.addEventListener('click', (e) => {
        if (e.isTrusted) finish({state: 'cancelled'});
      });
      picker.appendChild(cancel);
      stage.setAttribute('picking', '');

      // Below the region if it fits, else above, else at its bottom right.
      // The whole page: under the hint, in the middle.
      const r = s.full ? {x: window.innerWidth / 2 + 120, y: 40, width: 0,
                          height: 16} :
                         s.rect;
      const w = picker.offsetWidth || 240;
      const h = picker.offsetHeight || 200;
      const gap = 8;
      let top = r.y + r.height + gap;
      if (top + h > window.innerHeight) top = r.y - h - gap;
      if (top < 0) top = r.y + r.height - h - gap;
      let left = r.x + r.width - w;
      left = Math.max(gap, Math.min(left, window.innerWidth - w - gap));
      top = Math.max(gap, Math.min(top, window.innerHeight - h - gap));
      picker.style.left = `${left}px`;
      picker.style.top = `${top}px`;
      const first = list.querySelector('button');
      if (first) first.focus();
    }

    stage.addEventListener('pointerdown', (e) => {
      if (!e.isTrusted || e.button !== 0 || picker.contains(e.composedPath()[0])) {
        return;
      }
      e.preventDefault();
      s.full = false;
      stage.removeAttribute('picking');
      stage.setAttribute('selecting', '');
      s.from = {x: e.clientX, y: e.clientY};
      place(e.clientX, e.clientY, e.clientX, e.clientY);
      stage.setPointerCapture(e.pointerId);
    });
    stage.addEventListener('pointermove', (e) => {
      if (!e.isTrusted || !s.from) return;
      place(s.from.x, s.from.y, e.clientX, e.clientY);
    });
    stage.addEventListener('pointerup', (e) => {
      if (!e.isTrusted || !s.from) return;
      s.from = null;
      if (!s.rect || s.rect.width < MIN_SIZE || s.rect.height < MIN_SIZE) {
        s.rect = null;
        stage.removeAttribute('selecting');
        return;
      }
      showPicker();
    });
    stage.focus({preventScroll: true});
    return true;
  }

  // What the browser's poll gets: the user's choice once, then nothing.
  function poll() {
    if (result) {
      const value = result;
      result = null;
      return value;
    }
    return session || pending ? null : {state: 'gone'};
  }

  function cancel() {
    removeOverlay();
    result = null;
    return true;
  }

  function toast(text) {
    if (toastHost) toastHost.remove();
    const {host, root} = makeHost(TOAST_CSS);
    root.appendChild(el('div', 'toast', text));
    toastHost = host;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      host.remove();
      if (toastHost === host) toastHost = null;
    }, TOAST_MS);
    return true;
  }

  // Refresh: scrolls where the capture was taken; the page may stop short.
  function scrollTo(x, y) {
    window.scrollTo({left: x, top: y, behavior: 'instant'});
    return view();
  }

  return {start, poll, cancel, toast, probe: view, scrollTo};
})();
