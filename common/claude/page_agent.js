// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's page agent: reads a page for Claude and acts on it. It's shared by
// desktop and iOS so both give Claude the same view of a page
// (docs/design/claude.md):
//
// - Desktop evaluates it in an isolated world (CDP Page.createIsolatedWorld)
//   and sends clicks, keys and wheel events through the DevTools protocol,
//   using rect() for coordinates. The synthetic actions below are unused
//   there.
// - iOS evaluates it in WKContentWorld "claude" and uses the synthetic
//   actions (click, hover, type, key, scroll), whose events have
//   isTrusted == false.
//
// Installing twice is a no-op. Every entry point takes one options object and
// returns plain JSON: {ok: true, ...} or {ok: false, error}. Coordinates are
// CSS pixels relative to the top-level viewport.

(() => {
  'use strict';
  const VERSION = 1;
  if (globalThis.__compassAgent && globalThis.__compassAgent.version === VERSION) {
    return;
  }

  // ---- Refs --------------------------------------------------------------
  // A ref names an element for as long as the document lives. The same element
  // keeps its ref across reads, so Claude can read once and act several times.

  let nextRef = 1;
  const refToElement = new Map();      // "e12" -> WeakRef<Element>
  const elementToRef = new WeakMap();  // Element -> "e12"

  function refFor(el) {
    let ref = elementToRef.get(el);
    if (!ref) {
      ref = 'e' + nextRef++;
      elementToRef.set(el, ref);
      refToElement.set(ref, new WeakRef(el));
    }
    return ref;
  }

  function elementFor(ref) {
    const el = refToElement.get(String(ref || '').trim())?.deref();
    if (!el || !el.isConnected) {
      throw new Error(`No element with ref ${ref}: the page changed. Read it again.`);
    }
    return el;
  }

  // ---- Roles and names ---------------------------------------------------

  const INTERACTIVE_ROLES = new Set([
    'button', 'checkbox', 'combobox', 'link', 'listbox', 'menuitem',
    'menuitemcheckbox', 'menuitemradio', 'option', 'radio', 'searchbox',
    'slider', 'spinbutton', 'switch', 'tab', 'textbox', 'treeitem',
    'clickable',
  ]);
  // Roles whose name comes from their content: they're written as one line and
  // their subtree isn't walked (unless it holds controls of its own).
  const NAME_FROM_CONTENT = new Set([
    'button', 'link', 'heading', 'menuitem', 'menuitemcheckbox',
    'menuitemradio', 'option', 'tab', 'treeitem', 'columnheader',
    'rowheader', 'cell', 'switch', 'clickable', 'label',
  ]);
  // Blocks of text: written as one line with their text when they hold no
  // controls, so inline markup (<b>, <span>) doesn't split them up.
  const TEXT_BLOCKS = new Set([
    'paragraph', 'listitem', 'blockquote', 'code', 'caption', 'term',
    'definition', 'note', 'status', 'alert',
  ]);
  // Elements whose presence makes a generic block more than a run of text.
  const STRUCTURE_SELECTOR =
      'h1, h2, h3, h4, h5, h6, p, ul, ol, li, table, img, iframe, video, ' +
      'audio, canvas, form, nav, main, aside, dialog, pre, blockquote, [role]';
  const SKIP_TAGS = new Set([
    'SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'HEAD', 'META', 'LINK', 'BR',
  ]);
  const TEXT_INPUT_TYPES = new Set([
    'text', 'email', 'password', 'search', 'tel', 'url', 'date',
    'datetime-local', 'month', 'week', 'time', 'color', '',
  ]);

  function implicitRole(el) {
    const tag = el.tagName;
    switch (tag) {
      case 'A': case 'AREA': return el.hasAttribute('href') ? 'link' : null;
      case 'BUTTON': return 'button';
      case 'SUMMARY': return 'button';
      case 'SELECT': return (el.multiple || el.size > 1) ? 'listbox' : 'combobox';
      case 'OPTION': return 'option';
      case 'TEXTAREA': return 'textbox';
      case 'INPUT': {
        const type = (el.getAttribute('type') || '').toLowerCase();
        if (type === 'hidden') return null;
        if (['button', 'submit', 'reset', 'image'].includes(type)) return 'button';
        if (type === 'checkbox') return 'checkbox';
        if (type === 'radio') return 'radio';
        if (type === 'range') return 'slider';
        if (type === 'number') return 'spinbutton';
        if (type === 'search') return 'searchbox';
        if (type === 'file') return 'button';
        return 'textbox';
      }
      case 'H1': case 'H2': case 'H3': case 'H4': case 'H5': case 'H6':
        return 'heading';
      case 'IMG': return el.getAttribute('alt') === '' ? null : 'img';
      case 'SVG': return null;
      case 'NAV': return 'navigation';
      case 'MAIN': return 'main';
      case 'ASIDE': return 'complementary';
      case 'HEADER': return el.closest('article, aside, main, nav, section') ? null : 'banner';
      case 'FOOTER': return el.closest('article, aside, main, nav, section') ? null : 'contentinfo';
      case 'FORM': return 'form';
      case 'DIALOG': return 'dialog';
      case 'TABLE': return 'table';
      case 'TR': return 'row';
      case 'TD': return 'cell';
      case 'TH': return el.closest('thead') ? 'columnheader' : 'rowheader';
      case 'UL': case 'OL': return 'list';
      case 'LI': return 'listitem';
      case 'P': return 'paragraph';
      case 'BLOCKQUOTE': return 'blockquote';
      case 'PRE': return 'code';
      case 'IFRAME': case 'FRAME': return 'iframe';
      case 'VIDEO': return 'video';
      case 'AUDIO': return 'audio';
      case 'CANVAS': return 'canvas';
      case 'LABEL': return 'label';
      case 'HR': return 'separator';
    }
    if (el.isContentEditable && !el.parentElement?.isContentEditable) {
      return 'textbox';
    }
    return null;
  }

  function roleOf(el, style) {
    const explicit = (el.getAttribute('role') || '').trim().split(/\s+/)[0];
    if (explicit && explicit !== 'presentation' && explicit !== 'none' &&
        explicit !== 'generic') {
      return explicit;
    }
    const implicit = implicitRole(el);
    if (implicit) return implicit;
    // Elements made clickable with script: a tab stop or a pointer cursor that
    // their parent doesn't have.
    const tabIndex = el.getAttribute('tabindex');
    if (tabIndex !== null && Number(tabIndex) >= 0) return 'clickable';
    if (el.hasAttribute('onclick')) return 'clickable';
    if (style && style.cursor === 'pointer' && el.parentElement &&
        getComputedStyle(el.parentElement).cursor !== 'pointer') {
      return 'clickable';
    }
    return null;
  }

  function collapse(text, max = 100) {
    const s = String(text || '').replace(/\s+/g, ' ').trim();
    return s.length > max ? s.slice(0, max - 1) + '…' : s;
  }

  function textOf(node) {
    // innerText respects CSS (hidden text, line breaks) but only exists on
    // HTML elements.
    return (node.innerText !== undefined ? node.innerText : node.textContent) || '';
  }

  function nameOf(el, role) {
    const labelledBy = el.getAttribute('aria-labelledby');
    if (labelledBy) {
      const root = el.getRootNode();
      const text = labelledBy.split(/\s+/)
          .map((id) => root.getElementById?.(id) || document.getElementById(id))
          .filter(Boolean).map(textOf).join(' ');
      if (text.trim()) return collapse(text);
    }
    const label = el.getAttribute('aria-label');
    if (label && label.trim()) return collapse(label);
    if (el.labels && el.labels.length) {
      const text = Array.from(el.labels).map(textOf).join(' ');
      if (text.trim()) return collapse(text);
    }
    if (el.tagName === 'IMG' || el.tagName === 'AREA') {
      return collapse(el.getAttribute('alt') || el.getAttribute('title'));
    }
    if (el.tagName === 'INPUT') {
      const type = (el.getAttribute('type') || '').toLowerCase();
      if (['button', 'submit', 'reset'].includes(type)) {
        return collapse(el.value || (type === 'submit' ? 'Submit' : type));
      }
      if (type === 'image') return collapse(el.alt || 'Submit');
      return collapse(el.getAttribute('placeholder') || el.getAttribute('title'));
    }
    if (el.tagName === 'TEXTAREA' || el.tagName === 'SELECT') {
      return collapse(el.getAttribute('placeholder') || el.getAttribute('title'));
    }
    if (el.tagName === 'IFRAME') return collapse(el.title || el.name);
    if (TEXT_BLOCKS.has(role) && !hasInteractiveDescendant(el)) {
      return collapse(textOf(el), 300);
    }
    if (NAME_FROM_CONTENT.has(role)) {
      const text = collapse(textOf(el));
      if (text) return text;
      // An icon-only control: look for a labelled image inside.
      const img = el.querySelector('img[alt], [aria-label], svg title');
      if (img) {
        return collapse(img.getAttribute('alt') || img.getAttribute('aria-label') ||
                        img.textContent);
      }
    }
    return collapse(el.getAttribute('title'));
  }

  function isVisible(el, style) {
    if (el.checkVisibility) {
      if (!el.checkVisibility({visibilityProperty: true, checkVisibilityCSS: true})) {
        return false;
      }
    } else if (style.display === 'none' || style.visibility === 'hidden') {
      return false;
    }
    if (el.getClientRects().length === 0 && style.display !== 'contents') {
      return false;
    }
    return true;
  }

  function stateOf(el, role) {
    const parts = [];
    if (role === 'heading') {
      const level = el.getAttribute('aria-level') || el.tagName.match(/^H(\d)$/)?.[1];
      if (level) parts.push(`level=${level}`);
    }
    if (role === 'link') {
      const href = el.getAttribute('href') || '';
      if (href && !href.startsWith('javascript:')) parts.push(`href=${collapse(href, 120)}`);
    }
    if (el.tagName === 'INPUT') {
      const type = (el.getAttribute('type') || 'text').toLowerCase();
      if (type !== 'text' && role === 'textbox') parts.push(`type=${type}`);
    }
    if (role === 'textbox' || role === 'searchbox' || role === 'spinbutton' ||
        role === 'slider' || role === 'combobox') {
      let value;
      if (el.tagName === 'SELECT') {
        value = Array.from(el.selectedOptions).map((o) => o.text).join(', ');
      } else if ('value' in el && el.type !== 'password') {
        value = el.value;
      } else if (el.type === 'password') {
        value = el.value ? '••••' : '';
      } else if (el.isContentEditable) {
        value = textOf(el);
      }
      if (value) parts.push(`value="${collapse(value, 80)}"`);
      if (el.tagName === 'SELECT') {
        const options = Array.from(el.options).slice(0, 20).map((o) => collapse(o.text, 40));
        if (el.options.length > 20) options.push(`… ${el.options.length - 20} more`);
        parts.push(`options=[${options.join(' | ')}]`);
      }
      const placeholder = el.getAttribute('placeholder');
      if (placeholder && !value) parts.push(`placeholder="${collapse(placeholder, 60)}"`);
    }
    if (role === 'checkbox' || role === 'radio' || role === 'switch' ||
        role === 'menuitemcheckbox' || role === 'menuitemradio') {
      const checked = 'checked' in el ? el.checked :
                      el.getAttribute('aria-checked') === 'true';
      parts.push(checked ? 'checked' : 'unchecked');
    }
    if (el.disabled || el.getAttribute('aria-disabled') === 'true') parts.push('disabled');
    if (el.required || el.getAttribute('aria-required') === 'true') parts.push('required');
    const expanded = el.getAttribute('aria-expanded');
    if (expanded !== null) parts.push(expanded === 'true' ? 'expanded' : 'collapsed');
    if (el.tagName === 'DETAILS') parts.push(el.open ? 'expanded' : 'collapsed');
    if (el.getAttribute('aria-selected') === 'true' || (el.tagName === 'OPTION' && el.selected)) {
      parts.push('selected');
    }
    if (el.getAttribute('aria-current') && el.getAttribute('aria-current') !== 'false') {
      parts.push('current');
    }
    if (el.getAttribute('aria-invalid') === 'true') parts.push('invalid');
    const r = el.getBoundingClientRect();
    if (r.bottom < 0 || r.top > innerHeight || r.right < 0 || r.left > innerWidth) {
      parts.push('offscreen');
    }
    return parts;
  }

  // ---- Walking (light DOM, open shadow roots, same-origin frames) --------

  function childNodesOf(node) {
    if (node.shadowRoot) return node.shadowRoot.childNodes;
    if (node.tagName === 'SLOT') {
      const assigned = node.assignedNodes({flatten: true});
      return assigned.length ? assigned : node.childNodes;
    }
    if (node.tagName === 'IFRAME' || node.tagName === 'FRAME') {
      try {
        const doc = node.contentDocument;
        if (doc && doc.body) return [doc.body];
      } catch (e) {}
      return [];
    }
    return node.childNodes;
  }

  function hasInteractiveDescendant(el) {
    return !!el.querySelector(
        'a[href], button, input:not([type=hidden]), select, textarea, ' +
        '[role=button], [role=link], [role=checkbox], [role=textbox], ' +
        '[tabindex]:not([tabindex="-1"]), [contenteditable=""], [contenteditable=true]');
  }

  class Writer {
    constructor(maxChars) {
      this.lines = [];
      this.length = 0;
      this.maxChars = maxChars;
      this.truncated = false;
    }
    add(line) {
      if (this.truncated) return false;
      if (this.length + line.length + 1 > this.maxChars) {
        this.truncated = true;
        return false;
      }
      this.lines.push(line);
      this.length += line.length + 1;
      return true;
    }
    get full() { return this.truncated; }
  }

  function quote(s) {
    return '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
  }

  function walk(node, depth, out, interactiveOnly) {
    if (out.full) return;
    if (node.nodeType === Node.TEXT_NODE) {
      if (interactiveOnly) return;
      const text = collapse(node.textContent, 300);
      if (text) out.add(`${'  '.repeat(depth)}- text: ${text}`);
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const el = node;
    if (SKIP_TAGS.has(el.tagName)) return;
    if (el.getAttribute('aria-hidden') === 'true') return;
    const style = getComputedStyle(el);
    if (!isVisible(el, style)) return;

    const role = roleOf(el, style);
    const interactive = role && INTERACTIVE_ROLES.has(role);
    let childDepth = depth;
    let walkChildren = true;

    const emit = interactiveOnly ? interactive : role && role !== 'label';
    if (emit) {
      const name = nameOf(el, role);
      const state = stateOf(el, role);
      let line = `${'  '.repeat(depth)}- ${role}`;
      if (name) line += ' ' + quote(name);
      line += ` [ref=${refFor(el)}]`;
      if (state.length) line += ' ' + state.join(' ');
      if (role === 'iframe' && !childNodesOf(el).length) line += ' (cross-origin, not readable)';
      if (!out.add(line)) return;
      childDepth = interactiveOnly ? depth : depth + 1;
      if ((NAME_FROM_CONTENT.has(role) || TEXT_BLOCKS.has(role)) &&
          !hasInteractiveDescendant(el)) {
        walkChildren = false;
      }
      if (role === 'textbox' || role === 'searchbox' || role === 'combobox' ||
          role === 'listbox' && el.tagName === 'SELECT' || role === 'img') {
        walkChildren = role === 'listbox';
      }
    }
    if (!walkChildren) return;
    // A label's text is already its control's name.
    const skipText = el.tagName === 'LABEL' && el.control;
    if (!emit && !skipText && !interactiveOnly && !el.shadowRoot && el.tagName !== 'SLOT' &&
        !el.querySelector(STRUCTURE_SELECTOR) && !hasInteractiveDescendant(el)) {
      // A generic element holding only text: one line.
      const text = collapse(textOf(el), 300);
      if (text) out.add(`${'  '.repeat(depth)}- text: ${text}`);
      return;
    }
    for (const child of childNodesOf(el)) {
      if (skipText && child.nodeType === Node.TEXT_NODE) continue;
      walk(child, childDepth, out, interactiveOnly);
      if (out.full) return;
    }
  }

  function deepActiveElement() {
    let el = document.activeElement;
    while (el) {
      if (el.shadowRoot && el.shadowRoot.activeElement) {
        el = el.shadowRoot.activeElement;
      } else if ((el.tagName === 'IFRAME' || el.tagName === 'FRAME')) {
        let inner = null;
        try { inner = el.contentDocument?.activeElement; } catch (e) {}
        if (!inner || inner === el.contentDocument.body) break;
        el = inner;
      } else {
        break;
      }
    }
    return el && el !== document.body ? el : null;
  }

  function pageInfo() {
    return {
      title: document.title,
      url: location.href,
      viewport: {
        width: innerWidth, height: innerHeight,
        scrollX: Math.round(scrollX), scrollY: Math.round(scrollY),
        scrollWidth: document.documentElement.scrollWidth,
        scrollHeight: document.documentElement.scrollHeight,
        devicePixelRatio,
      },
    };
  }

  // ---- Geometry ----------------------------------------------------------

  // The element's box in top-level viewport coordinates, adding the offsets of
  // same-origin frames it sits in.
  function viewportRect(el) {
    const r = el.getBoundingClientRect();
    let x = r.left, y = r.top;
    let win = el.ownerDocument.defaultView;
    while (win && win !== window && win.frameElement) {
      const fr = win.frameElement.getBoundingClientRect();
      const fs = getComputedStyle(win.frameElement);
      x += fr.left + parseFloat(fs.borderLeftWidth) + parseFloat(fs.paddingLeft);
      y += fr.top + parseFloat(fs.borderTopWidth) + parseFloat(fs.paddingTop);
      win = win.parent;
    }
    return {x, y, width: r.width, height: r.height};
  }

  function scrollIntoViewIfNeeded(el) {
    const r = viewportRect(el);
    if (r.y < 0 || r.x < 0 || r.y + r.height > innerHeight || r.x + r.width > innerWidth) {
      el.scrollIntoView({block: 'center', inline: 'center', behavior: 'instant'});
    }
  }

  // The point a click on el lands on: its center, or, for a wrapped inline
  // element, the center of its first line box.
  function clickPoint(el) {
    const rects = el.getClientRects();
    const box = rects.length ? rects[0] : el.getBoundingClientRect();
    const r = viewportRect(el);
    const dx = box.left - el.getBoundingClientRect().left;
    const dy = box.top - el.getBoundingClientRect().top;
    return {x: Math.round(r.x + dx + box.width / 2), y: Math.round(r.y + dy + box.height / 2)};
  }

  // The topmost element at a top-level viewport point, descending into open
  // shadow roots and same-origin frames.
  function deepElementFromPoint(x, y) {
    let doc = document, px = x, py = y, el = null;
    for (;;) {
      el = doc.elementFromPoint(px, py);
      while (el && el.shadowRoot) {
        const inner = el.shadowRoot.elementFromPoint(px, py);
        if (!inner || inner === el) break;
        el = inner;
      }
      if (el && (el.tagName === 'IFRAME' || el.tagName === 'FRAME')) {
        let inner = null;
        try { inner = el.contentDocument; } catch (e) {}
        if (!inner) return el;
        const fr = el.getBoundingClientRect();
        px -= fr.left; py -= fr.top;
        doc = inner;
        continue;
      }
      return el;
    }
  }

  function describe(el) {
    if (!el) return null;
    const role = roleOf(el, getComputedStyle(el)) || el.tagName.toLowerCase();
    return {ref: refFor(el), role, name: nameOf(el, role)};
  }

  function targetOf(opts) {
    if (opts.ref) {
      const el = elementFor(opts.ref);
      scrollIntoViewIfNeeded(el);
      const p = clickPoint(el);
      const hit = deepElementFromPoint(p.x, p.y);
      // A real click lands on whatever is on top; say so when it isn't the
      // element asked for (a cookie banner, an overlay).
      const covered = hit && hit !== el && !el.contains(hit) && !hit.contains(el);
      return {el: covered ? hit : el, x: p.x, y: p.y, covered: covered ? describe(hit) : null};
    }
    if (Array.isArray(opts.coordinate)) {
      const [x, y] = opts.coordinate.map(Number);
      const el = deepElementFromPoint(x, y);
      if (!el) throw new Error(`Nothing at (${x}, ${y}).`);
      return {el, x, y, covered: null};
    }
    throw new Error('Give a ref or a coordinate.');
  }

  // ---- Synthetic input (iOS) ----------------------------------------------

  function modifierFlags(spec) {
    const keys = String(spec || '').toLowerCase().split('+').map((s) => s.trim());
    return {
      shiftKey: keys.includes('shift'),
      ctrlKey: keys.includes('ctrl') || keys.includes('control'),
      altKey: keys.includes('alt') || keys.includes('option'),
      metaKey: keys.includes('cmd') || keys.includes('meta') || keys.includes('command'),
    };
  }

  function mouseInit(t, button, mods, detail) {
    return {
      bubbles: true, cancelable: true, composed: true, view: t.el.ownerDocument.defaultView,
      clientX: t.x, clientY: t.y, screenX: t.x, screenY: t.y,
      button, buttons: button === 0 ? 1 : button === 2 ? 2 : 4, detail, ...mods,
    };
  }

  function fire(el, Type, type, init) {
    const Ctor = el.ownerDocument.defaultView[Type] || globalThis[Type] || MouseEvent;
    return el.dispatchEvent(new Ctor(type, init));
  }

  function focusable(el) {
    return el.matches?.(
        'a[href], button, input, select, textarea, summary, [tabindex], ' +
        '[contenteditable=""], [contenteditable=true]') && !el.disabled;
  }

  function synthClick(opts) {
    const t = targetOf(opts);
    const button = {left: 0, middle: 1, right: 2}[opts.button || 'left'] ?? 0;
    const mods = modifierFlags(opts.modifiers);
    const count = Math.min(Math.max(Number(opts.click_count) || 1, 1), 3);
    const pointer = {...mouseInit(t, button, mods, 1), pointerId: 1, pointerType: 'mouse',
                     isPrimary: true};
    fire(t.el, 'PointerEvent', 'pointerover', pointer);
    fire(t.el, 'MouseEvent', 'mouseover', mouseInit(t, button, mods, 0));
    for (let i = 1; i <= count; i++) {
      fire(t.el, 'PointerEvent', 'pointerdown', pointer);
      const down = fire(t.el, 'MouseEvent', 'mousedown', mouseInit(t, button, mods, i));
      if (down && i === 1) {
        const target = t.el.closest?.('a[href], button, input, select, textarea, summary, ' +
                                      '[tabindex], [contenteditable]') || t.el;
        if (focusable(target)) target.focus({preventScroll: true});
      }
      fire(t.el, 'PointerEvent', 'pointerup', {...pointer, buttons: 0});
      fire(t.el, 'MouseEvent', 'mouseup', {...mouseInit(t, button, mods, i), buttons: 0});
      if (button === 0) {
        fire(t.el, 'MouseEvent', 'click', {...mouseInit(t, button, mods, i), buttons: 0});
      } else if (button === 1) {
        fire(t.el, 'MouseEvent', 'auxclick', {...mouseInit(t, button, mods, i), buttons: 0});
      }
    }
    if (button === 0 && count === 2) {
      fire(t.el, 'MouseEvent', 'dblclick', {...mouseInit(t, button, mods, 2), buttons: 0});
    }
    if (button === 2) fire(t.el, 'MouseEvent', 'contextmenu', mouseInit(t, button, mods, 1));
    return {clicked: describe(t.el), x: t.x, y: t.y, covered: t.covered};
  }

  function synthHover(opts) {
    const t = targetOf(opts);
    const init = mouseInit(t, 0, {}, 0);
    init.buttons = 0;
    const pointer = {...init, pointerId: 1, pointerType: 'mouse', isPrimary: true};
    fire(t.el, 'PointerEvent', 'pointerover', pointer);
    fire(t.el, 'PointerEvent', 'pointerenter', {...pointer, bubbles: false});
    fire(t.el, 'MouseEvent', 'mouseover', init);
    fire(t.el, 'MouseEvent', 'mouseenter', {...init, bubbles: false});
    fire(t.el, 'PointerEvent', 'pointermove', pointer);
    fire(t.el, 'MouseEvent', 'mousemove', init);
    return {hovered: describe(t.el), x: t.x, y: t.y};
  }

  // Sets a field's value through the native setter, so frameworks that track
  // the value themselves (React) see the change.
  function setNativeValue(el, value) {
    const proto = Object.getPrototypeOf(el);
    const setter = Object.getOwnPropertyDescriptor(proto, 'value')?.set;
    if (setter) setter.call(el, value); else el.value = value;
  }

  function isTextField(el) {
    return el && (el.tagName === 'TEXTAREA' ||
        (el.tagName === 'INPUT' &&
         (TEXT_INPUT_TYPES.has((el.getAttribute('type') || '').toLowerCase()) ||
          el.type === 'number')));
  }

  function insertText(el, text) {
    if (isTextField(el)) {
      let start = null, end = null;
      try { start = el.selectionStart; end = el.selectionEnd; } catch (e) {}
      const value = el.value;
      if (start === null) { start = end = value.length; }
      if (!el.dispatchEvent(new InputEvent('beforeinput', {
            bubbles: true, cancelable: true, composed: true,
            inputType: 'insertText', data: text}))) {
        return;
      }
      setNativeValue(el, value.slice(0, start) + text + value.slice(end));
      try { el.setSelectionRange(start + text.length, start + text.length); } catch (e) {}
      el.dispatchEvent(new InputEvent('input', {
        bubbles: true, composed: true, inputType: 'insertText', data: text}));
    } else if (el && el.isContentEditable) {
      el.ownerDocument.execCommand('insertText', false, text);
    } else {
      throw new Error('The focused element doesn\'t take text. Click a field first, or give its ref.');
    }
  }

  function deleteText(el, forward) {
    if (isTextField(el)) {
      let start = el.selectionStart, end = el.selectionEnd;
      if (start === null) return;
      if (start === end) {
        if (forward) end = Math.min(end + 1, el.value.length);
        else start = Math.max(start - 1, 0);
      }
      setNativeValue(el, el.value.slice(0, start) + el.value.slice(end));
      el.setSelectionRange(start, start);
      el.dispatchEvent(new InputEvent('input', {
        bubbles: true, composed: true,
        inputType: forward ? 'deleteContentForward' : 'deleteContentBackward'}));
    } else if (el && el.isContentEditable) {
      el.ownerDocument.execCommand(forward ? 'forwardDelete' : 'delete');
    }
  }

  function synthType(opts) {
    let el;
    if (opts.ref) {
      el = elementFor(opts.ref);
      scrollIntoViewIfNeeded(el);
      el.focus({preventScroll: true});
    } else {
      el = deepActiveElement();
    }
    if (!el) throw new Error('Nothing is focused. Click a field first, or give its ref.');
    insertText(el, String(opts.text ?? ''));
    return {typed_into: describe(el)};
  }

  const KEY_ALIASES = {
    enter: 'Enter', return: 'Enter', tab: 'Tab', escape: 'Escape', esc: 'Escape',
    backspace: 'Backspace', delete: 'Delete', space: ' ', up: 'ArrowUp',
    down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight', arrowup: 'ArrowUp',
    arrowdown: 'ArrowDown', arrowleft: 'ArrowLeft', arrowright: 'ArrowRight',
    home: 'Home', end: 'End', pageup: 'PageUp', pagedown: 'PageDown',
  };
  const MODIFIER_NAMES = new Set(['shift', 'ctrl', 'control', 'alt', 'option', 'cmd',
                                  'meta', 'command']);

  function parseCombo(combo) {
    const parts = combo.split('+');
    const keyPart = parts.pop();
    const mods = modifierFlags(parts.join('+'));
    if (MODIFIER_NAMES.has(keyPart.toLowerCase())) {
      return {key: keyPart, mods, modifierOnly: true};
    }
    const key = KEY_ALIASES[keyPart.toLowerCase()] ||
                (keyPart.length === 1 ? keyPart : keyPart[0].toUpperCase() + keyPart.slice(1));
    return {key, mods, modifierOnly: false};
  }

  function codeFor(key) {
    if (/^[a-z]$/i.test(key)) return 'Key' + key.toUpperCase();
    if (/^[0-9]$/.test(key)) return 'Digit' + key;
    if (key === ' ') return 'Space';
    return key;
  }

  function tabbables() {
    return Array.from(document.querySelectorAll(
        'a[href], button, input:not([type=hidden]), select, textarea, summary, ' +
        '[tabindex]:not([tabindex="-1"]), [contenteditable=""], [contenteditable=true]'))
        .filter((el) => !el.disabled && isVisible(el, getComputedStyle(el)));
  }

  // What the browser would do for a key the page didn't handle.
  function defaultKeyAction(el, key, mods) {
    const shortcut = mods.metaKey || mods.ctrlKey;
    const editable = isTextField(el) || el?.isContentEditable;
    if (shortcut) {
      if (key.toLowerCase() === 'a') {
        if (isTextField(el)) el.select();
        else (el?.ownerDocument || document).execCommand('selectAll');
      }
      return;
    }
    switch (key) {
      case 'Enter':
        if (el?.tagName === 'TEXTAREA' || el?.isContentEditable) {
          insertText(el, '\n');
        } else if (el?.tagName === 'INPUT' && el.form) {
          el.form.requestSubmit();
        } else if (el && el.matches?.('a[href], button, summary, [role=button], [role=link]')) {
          el.click();
        }
        return;
      case ' ':
        if (editable) insertText(el, ' ');
        else if (el && el.matches?.('button, summary, [role=button], input[type=checkbox], ' +
                                    'input[type=radio]')) el.click();
        else scrollBy({top: innerHeight * 0.8 * (mods.shiftKey ? -1 : 1), behavior: 'instant'});
        return;
      case 'Tab': {
        const list = tabbables();
        const i = list.indexOf(el);
        const next = list[(i + (mods.shiftKey ? -1 : 1) + list.length) % list.length];
        next?.focus();
        return;
      }
      case 'Backspace': if (editable) deleteText(el, false); return;
      case 'Delete': if (editable) deleteText(el, true); return;
      case 'ArrowDown': case 'ArrowUp':
        if (!editable) scrollBy({top: key === 'ArrowDown' ? 40 : -40, behavior: 'instant'});
        return;
      case 'PageDown': case 'PageUp':
        if (!editable) scrollBy({top: innerHeight * 0.8 * (key === 'PageDown' ? 1 : -1),
                                 behavior: 'instant'});
        return;
      case 'Home': case 'End':
        if (!editable) scrollTo({top: key === 'Home' ? 0 : document.documentElement.scrollHeight,
                                 behavior: 'instant'});
        return;
    }
    if (key.length === 1 && editable) insertText(el, mods.shiftKey ? key.toUpperCase() : key);
  }

  function synthKey(opts) {
    const combos = String(opts.keys || '').trim().split(/\s+/).filter(Boolean);
    if (!combos.length) throw new Error('No keys given.');
    const repeat = Math.min(Math.max(Number(opts.repeat) || 1, 1), 50);
    for (let r = 0; r < repeat; r++) {
      for (const combo of combos) {
        const {key, mods, modifierOnly} = parseCombo(combo);
        const el = deepActiveElement() || document.body;
        const init = {key, code: codeFor(key), bubbles: true, cancelable: true, composed: true,
                      ...mods};
        const notCancelled = el.dispatchEvent(new KeyboardEvent('keydown', init));
        if (notCancelled && !modifierOnly) {
          if (key.length === 1 && !mods.metaKey && !mods.ctrlKey) {
            el.dispatchEvent(new KeyboardEvent('keypress', init));
          }
          defaultKeyAction(el === document.body ? null : el, key, mods);
        }
        (deepActiveElement() || document.body).dispatchEvent(new KeyboardEvent('keyup', init));
      }
    }
    return {focused: describe(deepActiveElement())};
  }

  function scrollableAncestor(el, vertical) {
    for (let n = el; n && n !== document.documentElement; n = n.parentElement ||
         n.getRootNode()?.host) {
      if (!(n instanceof Element)) continue;
      const s = getComputedStyle(n);
      const overflow = vertical ? s.overflowY : s.overflowX;
      const room = vertical ? n.scrollHeight > n.clientHeight : n.scrollWidth > n.clientWidth;
      if (room && /(auto|scroll|overlay)/.test(overflow)) return n;
    }
    return document.scrollingElement || document.documentElement;
  }

  function synthScroll(opts) {
    const vertical = opts.direction === 'up' || opts.direction === 'down';
    const sign = opts.direction === 'up' || opts.direction === 'left' ? -1 : 1;
    const amount = Math.min(Math.max(Number(opts.amount) || 3, 1), 20);
    let from = null;
    if (opts.ref) from = elementFor(opts.ref);
    else if (Array.isArray(opts.coordinate)) from = deepElementFromPoint(...opts.coordinate);
    const scroller = from ? scrollableAncestor(from, vertical) :
                     (document.scrollingElement || document.documentElement);
    const isRoot = scroller === document.scrollingElement ||
                   scroller === document.documentElement;
    const page = vertical ? (isRoot ? innerHeight : scroller.clientHeight) :
                            (isRoot ? innerWidth : scroller.clientWidth);
    const delta = sign * amount * Math.round(page / 3);
    const before = vertical ? scroller.scrollTop : scroller.scrollLeft;
    scroller.scrollBy({[vertical ? 'top' : 'left']: delta, behavior: 'instant'});
    const after = vertical ? scroller.scrollTop : scroller.scrollLeft;
    return {scrolled: Math.round(after - before), position: Math.round(after),
            max: Math.round(vertical ? scroller.scrollHeight - scroller.clientHeight :
                                       scroller.scrollWidth - scroller.clientWidth),
            container: isRoot ? 'page' : describe(scroller)};
  }

  // ---- Entry points ------------------------------------------------------

  function wrap(fn) {
    return (opts) => {
      try {
        return {ok: true, ...fn(opts || {})};
      } catch (e) {
        return {ok: false, error: String(e && e.message || e)};
      }
    };
  }

  const api = {
    version: VERSION,

    readPage: wrap((opts) => {
      const out = new Writer(Math.min(Math.max(Number(opts.max_chars) || 20000, 1000), 100000));
      const root = opts.ref ? elementFor(opts.ref) : document.body;
      if (!root) throw new Error('The page has no body yet.');
      walk(root, 0, out, opts.filter === 'interactive');
      const focused = deepActiveElement();
      return {
        ...pageInfo(),
        tree: out.lines.join('\n') +
              (out.truncated ? '\n… (cut: read a subtree with ref, or use find)' : ''),
        truncated: out.truncated,
        focused: focused ? refFor(focused) : null,
      };
    }),

    pageText: wrap((opts) => {
      const max = Math.min(Math.max(Number(opts.max_chars) || 50000, 1000), 200000);
      let root = document.querySelector('main, [role=main], article');
      if (!root || textOf(root).trim().length < 500) root = document.body;
      let text = textOf(root || document.documentElement)
          .replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n').trim();
      const truncated = text.length > max;
      if (truncated) text = text.slice(0, max) + '\n… (cut)';
      return {...pageInfo(), text, truncated};
    }),

    find: wrap((opts) => {
      const query = String(opts.query || '').toLowerCase().trim();
      if (!query) throw new Error('Empty query.');
      const max = Math.min(Math.max(Number(opts.max_results) || 20, 1), 100);
      const matches = [];
      const visit = (node) => {
        if (matches.length >= max || node.nodeType !== Node.ELEMENT_NODE) return;
        if (SKIP_TAGS.has(node.tagName) || node.getAttribute('aria-hidden') === 'true') return;
        const style = getComputedStyle(node);
        if (!isVisible(node, style)) return;
        let role = roleOf(node, style);
        if (role === 'label' && node.control) role = null;
        if (role) {
          const name = nameOf(node, role);
          let value = 'value' in node && node.type !== 'password' ? String(node.value || '') : '';
          if (node.tagName === 'SELECT') {
            value += ' ' + Array.from(node.options).map((o) => o.text).join(' ');
          }
          const own = NAME_FROM_CONTENT.has(role) ? '' : Array.from(node.childNodes)
              .filter((c) => c.nodeType === Node.TEXT_NODE).map((c) => c.textContent).join(' ');
          const haystack = `${name} ${value} ${own} ${node.getAttribute('placeholder') || ''}`
              .toLowerCase();
          if (haystack.includes(query)) {
            matches.push({ref: refFor(node), role, name,
                          state: stateOf(node, role).join(' ')});
            if (NAME_FROM_CONTENT.has(role) || node.tagName === 'SELECT') return;
          }
          if (node.tagName === 'SELECT') return;
        } else if (node.tagName !== 'LABEL') {
          // A generic element counts when one of its own text nodes matches.
          const own = Array.from(node.childNodes)
              .filter((c) => c.nodeType === Node.TEXT_NODE).map((c) => c.textContent).join(' ');
          if (own.toLowerCase().includes(query)) {
            matches.push({ref: refFor(node), role: 'text', name: collapse(own)});
          }
        }
        for (const child of childNodesOf(node)) visit(child);
      };
      if (document.body) visit(document.body);
      return {matches, more: matches.length >= max};
    }),

    // Where to click an element: its center in top-level viewport CSS pixels,
    // after scrolling it into view. Desktop sends CDP input there.
    rect: wrap((opts) => {
      const el = elementFor(opts.ref);
      if (opts.scroll !== false) scrollIntoViewIfNeeded(el);
      const r = viewportRect(el);
      const p = clickPoint(el);
      const hit = deepElementFromPoint(p.x, p.y);
      const covered = hit && hit !== el && !el.contains(hit) && !hit.contains(el);
      return {x: p.x, y: p.y, rect: r, element: describe(el),
              covered: covered ? describe(hit) : null,
              inViewport: p.x >= 0 && p.y >= 0 && p.x < innerWidth && p.y < innerHeight};
    }),

    elementAt: wrap((opts) => ({element: describe(deepElementFromPoint(opts.x, opts.y))})),

    focus: wrap((opts) => {
      const el = elementFor(opts.ref);
      scrollIntoViewIfNeeded(el);
      el.focus({preventScroll: true});
      return {focused: describe(deepActiveElement())};
    }),

    scrollIntoView: wrap((opts) => {
      elementFor(opts.ref).scrollIntoView({block: 'center', inline: 'center',
                                           behavior: 'instant'});
      return {};
    }),

    formInput: wrap((opts) => {
      let el = elementFor(opts.ref);
      if (el.tagName === 'LABEL' && el.control) el = el.control;
      const value = opts.value;
      scrollIntoViewIfNeeded(el);
      const fireChange = () => {
        el.dispatchEvent(new Event('input', {bubbles: true, composed: true}));
        el.dispatchEvent(new Event('change', {bubbles: true}));
      };
      if (el.tagName === 'SELECT') {
        const wanted = String(value);
        const option = Array.from(el.options).find((o) => o.value === wanted) ||
                       Array.from(el.options).find((o) => collapse(o.text) === collapse(wanted)) ||
                       Array.from(el.options).find((o) =>
                           o.text.toLowerCase().includes(wanted.toLowerCase()));
        if (!option) {
          throw new Error(`No option "${wanted}". Options: ` +
                          Array.from(el.options).map((o) => collapse(o.text, 40)).join(', '));
        }
        setNativeValue(el, option.value);
        fireChange();
        return {value: option.text};
      }
      if (el.tagName === 'INPUT' && (el.type === 'checkbox' || el.type === 'radio')) {
        const want = value === true || value === 'true' || value === 1 || value === 'on';
        if (el.checked !== want) el.click();
        return {value: el.checked};
      }
      if (isTextField(el)) {
        el.focus({preventScroll: true});
        setNativeValue(el, String(value));
        fireChange();
        return {value: el.type === 'password' ? '••••' : el.value};
      }
      if (el.isContentEditable) {
        el.focus({preventScroll: true});
        el.ownerDocument.execCommand('selectAll');
        el.ownerDocument.execCommand('insertText', false, String(value));
        return {value: collapse(textOf(el), 200)};
      }
      if (el.getAttribute('role') === 'checkbox' || el.getAttribute('role') === 'switch') {
        const want = value === true || value === 'true';
        if ((el.getAttribute('aria-checked') === 'true') !== want) synthClick({ref: opts.ref});
        return {value: el.getAttribute('aria-checked') === 'true'};
      }
      throw new Error('That element isn\'t a form field.');
    }),

    // Synthetic actions, used on iOS.
    click: wrap(synthClick),
    hover: wrap(synthHover),
    type: wrap(synthType),
    key: wrap(synthKey),
    scroll: wrap(synthScroll),
  };

  Object.defineProperty(globalThis, '__compassAgent', {
    value: Object.freeze(api), configurable: true, writable: false, enumerable: false,
  });
})();
