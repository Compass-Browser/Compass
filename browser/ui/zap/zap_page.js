// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Zap's side in the page (docs/design/boosts.md, "Zap"): hovering outlines
// an element, clicking hides it, and Done hands the browser a selector for
// each, which it keeps in the site's zap Boost. Runs in the isolated world
// of the main frame (zap_session.cc): the page's scripts can't see these
// functions or the bar's closed shadow root. The browser polls poll().
window.__compassZap = window.__compassZap || (() => {
  'use strict';

  const TOAST_MS = 2400;
  // Classes that look made by a build (hashes, numbers) change between
  // deploys: not used in selectors.
  const STABLE_CLASS = /^[A-Za-z][A-Za-z_-]*[A-Za-z0-9_-]{0,30}$/;
  const HASHY = /\d{3,}|[A-Z0-9]{6,}|__[a-z0-9]{5,}$/;
  const MAX_DEPTH = 10;

  let session = null;
  let result = null;
  let toastHost = null;
  let toastTimer = 0;

  function el(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }

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
      // No popover support: z-index it is.
    }
    return {host, root};
  }

  const BAR_CSS = `
    :host { color-scheme: light dark; }
    * { box-sizing: border-box; }
    .box {
      position: fixed; display: none; pointer-events: none;
      outline: 2px solid #e5484d; background: rgba(229, 72, 77, 0.18);
      border-radius: 2px;
    }
    .bar {
      position: fixed; left: 50%; bottom: 20px; transform: translateX(-50%);
      display: flex; align-items: center; gap: 8px; padding: 6px 6px 6px 14px;
      border-radius: 999px; pointer-events: auto; white-space: nowrap;
      background: rgba(32, 33, 36, 0.94); color: #fff;
      font: 13px/1.35 system-ui, -apple-system, "Segoe UI", sans-serif;
      box-shadow: 0 4px 16px rgba(0, 0, 0, 0.3);
    }
    button {
      margin: 0; padding: 5px 12px; border: 0; border-radius: 999px;
      background: rgba(255, 255, 255, 0.16); color: #fff; font: inherit;
      cursor: pointer;
    }
    button:hover, button:focus-visible {
      background: rgba(255, 255, 255, 0.28); outline: none;
    }
    button:disabled { opacity: 0.4; cursor: default; }
    button.done { background: #fff; color: #202124; font-weight: 600; }
  `;

  const TOAST_CSS = `
    .toast {
      position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%);
      max-width: min(480px, calc(100vw - 32px)); padding: 9px 16px;
      border-radius: 999px; background: rgba(32, 33, 36, 0.94); color: #fff;
      font: 13px/1.35 system-ui, -apple-system, "Segoe UI", sans-serif;
      overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
    }
  `;

  function goodId(id) {
    return id && id.length <= 40 && !HASHY.test(id) &&
        document.querySelectorAll('#' + CSS.escape(id)).length === 1;
  }

  // One step of a selector for |node|: its tag, a couple of stable classes,
  // and its place among its siblings when those aren't enough.
  function step(node) {
    let part = node.localName;
    const classes = [...node.classList]
                        .filter(c => STABLE_CLASS.test(c) && !HASHY.test(c))
                        .slice(0, 2);
    for (const c of classes) part += '.' + CSS.escape(c);
    const parent = node.parentElement;
    if (parent) {
      const same = [...parent.children].filter(
          sibling => sibling.localName === node.localName);
      if (same.length > 1 &&
          parent.querySelectorAll(':scope > ' + part).length > 1) {
        part += `:nth-of-type(${same.indexOf(node) + 1})`;
      }
    }
    return part;
  }

  // A selector that picks |target| alone: its id if it has a good one, else
  // steps up to an ancestor with one (or the body), as short as is unique.
  function selectorFor(target) {
    if (goodId(target.id)) return '#' + CSS.escape(target.id);
    const parts = [];
    for (let node = target, depth = 0;
         node && node !== document.body && node !== document.documentElement &&
         depth < MAX_DEPTH;
         node = node.parentElement, depth++) {
      if (depth > 0 && goodId(node.id)) {
        parts.unshift('#' + CSS.escape(node.id));
        break;
      }
      parts.unshift(step(node));
      const selector = parts.join(' > ');
      try {
        const found = document.querySelectorAll(selector);
        if (found.length === 1 && found[0] === target) return selector;
      } catch (e) {
        // Not a selector after all: keep going up.
      }
    }
    const selector = parts.join(' > ');
    try {
      const found = document.querySelectorAll(selector);
      if (found.length === 1 && found[0] === target) return selector;
    } catch (e) {
    }
    return 'body > ' + selector;
  }

  function hide(node) {
    const before = [node.style.getPropertyValue('display'),
                    node.style.getPropertyPriority('display')];
    node.style.setProperty('display', 'none', 'important');
    return before;
  }

  function show(node, before) {
    if (before[0]) {
      node.style.setProperty('display', before[0], before[1]);
    } else {
      node.style.removeProperty('display');
    }
  }

  function end(value) {
    if (!session) return;
    const s = session;
    session = null;
    window.removeEventListener('keydown', s.onKey, true);
    document.removeEventListener('pointermove', s.onMove, true);
    document.removeEventListener('click', s.onClick, true);
    document.removeEventListener('pointerdown', s.onSwallow, true);
    document.removeEventListener('mousedown', s.onSwallow, true);
    document.removeEventListener('mouseup', s.onSwallow, true);
    document.removeEventListener('pointerup', s.onSwallow, true);
    s.host.remove();
    if (value.state !== 'done') {
      for (let i = s.zapped.length - 1; i >= 0; i--) {
        show(s.zapped[i].node, s.zapped[i].before);
      }
    }
    result = value;
  }

  function start() {
    if (session) end({state: 'cancelled'});
    result = null;
    const {host, root} = makeHost(BAR_CSS);
    const box = el('div', 'box');
    const bar = el('div', 'bar');
    const label = el('span', '', 'Click what to hide · Esc to cancel');
    const undo = el('button', '', 'Undo');
    const done = el('button', 'done', 'Done');
    undo.disabled = true;
    bar.append(label, undo, done);
    root.append(box, bar);

    const s = {host, box, bar, zapped: [], hovered: null};
    session = s;

    function update() {
      undo.disabled = s.zapped.length === 0;
      label.textContent = s.zapped.length ?
          `${s.zapped.length} hidden · Enter when done, Esc to cancel` :
          'Click what to hide · Esc to cancel';
    }

    // The element under the pointer, not the bar.
    function under(x, y) {
      for (const node of document.elementsFromPoint(x, y)) {
        if (node === host) return null;
        if (node === document.documentElement || node === document.body) {
          return null;
        }
        return node;
      }
      return null;
    }

    function outline(node) {
      s.hovered = node;
      if (!node) {
        box.style.display = 'none';
        return;
      }
      const r = node.getBoundingClientRect();
      box.style.display = 'block';
      box.style.left = `${r.left}px`;
      box.style.top = `${r.top}px`;
      box.style.width = `${r.width}px`;
      box.style.height = `${r.height}px`;
    }

    function inBar(e) {
      return e.composedPath().includes(host);
    }

    s.onMove = (e) => {
      if (session !== s || !e.isTrusted || inBar(e)) return;
      outline(under(e.clientX, e.clientY));
    };
    // The page doesn't get the clicks meant for zapping.
    s.onSwallow = (e) => {
      if (session !== s || !e.isTrusted || inBar(e)) return;
      e.preventDefault();
      e.stopImmediatePropagation();
    };
    s.onClick = (e) => {
      if (session !== s || !e.isTrusted || inBar(e)) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      const node = under(e.clientX, e.clientY);
      if (!node) return;
      const selector = selectorFor(node);
      s.zapped.push({node, selector, before: hide(node)});
      outline(null);
      update();
    };
    // Esc cancels; Enter is Done.
    s.onKey = (e) => {
      if (session !== s || !e.isTrusted ||
          (e.key !== 'Escape' && e.key !== 'Enter')) {
        return;
      }
      e.preventDefault();
      e.stopImmediatePropagation();
      if (e.key === 'Escape') {
        end({state: 'cancelled'});
      } else {
        end({state: 'done', selectors: s.zapped.map(z => z.selector)});
      }
    };
    undo.addEventListener('click', (e) => {
      if (!e.isTrusted || session !== s) return;
      const last = s.zapped.pop();
      if (last) show(last.node, last.before);
      update();
    });
    done.addEventListener('click', (e) => {
      if (!e.isTrusted || session !== s) return;
      end({state: 'done', selectors: s.zapped.map(z => z.selector)});
    });

    window.addEventListener('keydown', s.onKey, true);
    document.addEventListener('pointermove', s.onMove, true);
    document.addEventListener('click', s.onClick, true);
    document.addEventListener('pointerdown', s.onSwallow, true);
    document.addEventListener('mousedown', s.onSwallow, true);
    document.addEventListener('mouseup', s.onSwallow, true);
    document.addEventListener('pointerup', s.onSwallow, true);
    return true;
  }

  function poll() {
    if (result) {
      const value = result;
      result = null;
      return value;
    }
    return session ? null : {state: 'gone'};
  }

  function cancel() {
    end({state: 'cancelled'});
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

  return {start, poll, cancel, toast, selectorFor};
})();
