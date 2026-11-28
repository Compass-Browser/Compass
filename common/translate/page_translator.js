// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The page translator every Compass platform runs in a page's frame, in
// Compass's own world (docs/design/translation.md, "The shared page
// translator"). It finds the page's text, asks an engine the platform gives
// it to translate it, and writes the translation back into the page's own
// nodes, so links, buttons and their listeners keep working. restore() puts
// the original back without a reload.
//
// Written for Compass; it follows the approach of Firefox's
// translations-document.sys.mjs without its code.
//
//   __compassTranslator.start({source, target, engine})
//     engine(segments) -> Promise<Array<string|null>>, a segment being
//     {text, html}: plain text, or a block's HTML whose elements carry
//     data-compass-t="n". null leaves that segment as it is.
//   __compassTranslator.stop()      stops, keeping what's translated
//   __compassTranslator.restore()   stops and puts the original back
//   __compassTranslator.progress()  {done, pending, failed}
//   __compassTranslator.idle()      Promise: nothing left to translate
//   __compassTranslator.started()   Promise: the first batch is back (the
//                                   first screen), or nothing to translate

(() => {
  'use strict';
  if (globalThis.__compassTranslator) {
    return;
  }

  // Elements whose content is never translated (their attributes may be).
  const SKIP = new Set([
    'SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'CODE', 'PRE', 'KBD', 'SAMP',
    'VAR', 'TEXTAREA', 'INPUT', 'SVG', 'MATH', 'IFRAME', 'CANVAS', 'VIDEO',
    'AUDIO', 'OBJECT', 'EMBED', 'HEAD',
  ]);
  // Elements that flow inside a sentence: kept with their text in one
  // segment, so the engine moves them with their words.
  const INLINE = new Set([
    'A', 'ABBR', 'B', 'BDI', 'BDO', 'BR', 'CITE', 'DATA', 'DFN', 'EM', 'FONT',
    'I', 'IMG', 'LABEL', 'MARK', 'Q', 'RUBY', 'RT', 'RP', 'S', 'SMALL',
    'SPAN', 'STRONG', 'SUB', 'SUP', 'TIME', 'U', 'WBR', 'CODE', 'KBD',
    'SAMP', 'VAR',
  ]);
  const ATTRIBUTES = [
    'title', 'alt', 'placeholder', 'aria-label', 'aria-description',
  ];
  const BUTTON_INPUTS = new Set(['button', 'submit', 'reset']);
  const ID = 'data-compass-t';
  const MAX_BATCH_SEGMENTS = 16;
  const MAX_BATCH_CHARS = 6000;
  const MAX_IN_FLIGHT = 2;

  // Translations by pair, kept for the page's life: translating again, or
  // after restore(), costs nothing.
  const caches = new Map();

  let state = null;

  // Text worth sending: some letters, and not just an address.
  function worthTranslating(text) {
    return /\p{L}/u.test(text) &&
        !/^\s*(?:https?:\/\/|www\.)\S+\s*$/i.test(text) &&
        !/^\s*[\w.+-]+@[\w-]+\.[\w.-]+\s*$/.test(text);
  }

  function skipped(element) {
    if (SKIP.has(element.tagName.toUpperCase())) {
      return true;
    }
    if (element.getAttribute('translate') === 'no' ||
        element.classList?.contains('notranslate') ||
        element.isContentEditable) {
      return true;
    }
    return false;
  }

  // Inside a translated sentence, its words stay as they are.
  function frozen(element) {
    return ['CODE', 'KBD', 'SAMP', 'VAR'].includes(element.tagName) ||
        element.getAttribute('translate') === 'no' ||
        element.classList?.contains('notranslate');
  }

  function isInlineOnly(element) {
    for (const child of element.children) {
      if (!INLINE.has(child.tagName) && !(child instanceof SVGElement)) {
        return false;
      }
      if (!frozen(child) && !isInlineOnly(child)) {
        return false;
      }
    }
    return true;
  }

  // The units under `root`: an element whose content is text and inline
  // elements, or a lone text node among blocks.
  function collectUnits(root, units) {
    if (root.nodeType === Node.TEXT_NODE) {
      if (worthTranslating(root.data) && root.parentElement &&
          !skipped(root.parentElement)) {
        units.push({kind: 'node', node: root});
      }
      return;
    }
    if (root.nodeType !== Node.ELEMENT_NODE &&
        root.nodeType !== Node.DOCUMENT_NODE &&
        root.nodeType !== Node.DOCUMENT_FRAGMENT_NODE) {
      return;
    }
    if (root.nodeType === Node.ELEMENT_NODE) {
      collectAttributes(root, units);
      if (skipped(root)) {
        return;
      }
      if (root.shadowRoot) {
        collectUnits(root.shadowRoot, units);
      }
      if (root.tagName === 'SELECT' || root.tagName === 'OPTGROUP') {
        for (const child of root.children) {
          collectUnits(child, units);
        }
        return;
      }
      if (root.tagName === 'OPTION') {
        if (worthTranslating(root.textContent)) {
          units.push({kind: 'text', element: root});
        }
        return;
      }
      if (root.tagName !== 'BODY' && worthTranslating(root.textContent) &&
          isInlineOnly(root)) {
        const elements = root.querySelectorAll('*');
        units.push({
          kind: elements.length ? 'html' : 'text',
          element: root,
        });
        // Attributes of the inline elements inside.
        for (const element of elements) {
          collectAttributes(element, units);
        }
        return;
      }
    }
    for (const child of root.childNodes) {
      collectUnits(child, units);
    }
  }

  function collectAttributes(element, units) {
    for (const name of ATTRIBUTES) {
      const value = element.getAttribute(name);
      if (value && worthTranslating(value)) {
        units.push({kind: 'attribute', element, name});
      }
    }
    if (element.tagName === 'INPUT' &&
        BUTTON_INPUTS.has((element.getAttribute('type') || '').toLowerCase()) &&
        worthTranslating(element.value)) {
      units.push({kind: 'attribute', element, name: 'value'});
    }
  }

  // What a unit sends, and what lets its answer be put back.
  function segmentOf(unit) {
    switch (unit.kind) {
      case 'node':
        return {text: unit.node.data, html: false};
      case 'text':
        return {text: unit.element.textContent, html: false};
      case 'attribute':
        return {
          text: unit.name === 'value' ? unit.element.value :
                                         unit.element.getAttribute(unit.name),
          html: false,
        };
      case 'html': {
        // A copy with numbered elements and nothing else: no attributes,
        // no comments, and frozen elements emptied.
        unit.elements = [...unit.element.querySelectorAll('*')];
        const copy = unit.element.cloneNode(true);
        const copies = [...copy.querySelectorAll('*')];
        copies.forEach((element, index) => {
          for (const name of [...element.getAttributeNames()]) {
            element.removeAttribute(name);
          }
          element.setAttribute(ID, String(index));
          if (frozen(unit.elements[index])) {
            element.textContent = '';
          }
        });
        const walker = document.createTreeWalker(copy, NodeFilter.SHOW_COMMENT);
        const comments = [];
        while (walker.nextNode()) {
          comments.push(walker.currentNode);
        }
        comments.forEach(comment => comment.remove());
        return {text: copy.innerHTML, html: true};
      }
    }
  }

  // Original values, for restore(): text nodes' text, attributes, and the
  // child lists of elements whose children were rearranged.
  function rememberText(node) {
    if (!state.originalText.has(node)) {
      state.originalText.set(node, node.data);
      state.touched.push(node);
    }
  }

  function writeText(node, text) {
    rememberText(node);
    node.data = text;
    state.written.set(node, text);
  }

  function apply(unit, translation) {
    switch (unit.kind) {
      case 'node':
        writeText(unit.node, translation);
        return;
      case 'text': {
        // An option without a value submits its text: keep submitting the
        // original.
        if (unit.element.tagName === 'OPTION' &&
            !unit.element.hasAttribute('value')) {
          let originals = state.originalAttributes.get(unit.element);
          if (!originals) {
            originals = new Map();
            state.originalAttributes.set(unit.element, originals);
            state.touched.push(unit.element);
          }
          originals.set('value', null);
          unit.element.setAttribute('value', unit.element.value);
        }
        const texts = [...unit.element.childNodes].filter(
            node => node.nodeType === Node.TEXT_NODE);
        if (texts.length === 1) {
          writeText(texts[0], translation);
        } else if (texts.length) {
          writeText(texts[0], translation);
          texts.slice(1).forEach(node => writeText(node, ''));
        }
        return;
      }
      case 'attribute': {
        const key = unit.name;
        let originals = state.originalAttributes.get(unit.element);
        if (!originals) {
          originals = new Map();
          state.originalAttributes.set(unit.element, originals);
          state.touched.push(unit.element);
        }
        if (unit.name === 'value') {
          if (!originals.has(key)) {
            originals.set(key, unit.element.value);
          }
          unit.element.value = translation;
        } else {
          if (!originals.has(key)) {
            originals.set(key, unit.element.getAttribute(key));
          }
          unit.element.setAttribute(key, translation);
        }
        return;
      }
      case 'html':
        applyHtml(unit, translation);
        return;
    }
  }

  // Puts a block's translated HTML back: the translation's text nodes into
  // the block's own text nodes, and the block's own elements (found by
  // their numbers) where the translation moved them.
  function applyHtml(unit, translation) {
    const template = document.createElement('template');
    template.innerHTML = translation;
    const translated = template.content;
    const byId = new Map();
    for (const element of translated.querySelectorAll(`[${ID}]`)) {
      byId.set(element.getAttribute(ID), element);
    }
    const rebuild = (live, model) => {
      if (live !== unit.element && frozen(live)) {
        return;  // Its words were never sent.
      }
      const ownTexts = [...live.childNodes].filter(
          node => node.nodeType === Node.TEXT_NODE);
      const ownElements = new Set(
          [...live.children].filter(child => unit.elements.includes(child)));
      const order = [];
      for (const child of model.childNodes) {
        if (child.nodeType === Node.TEXT_NODE) {
          order.push({text: child.data});
        } else if (child.nodeType === Node.ELEMENT_NODE &&
                   child.hasAttribute(ID)) {
          const element = unit.elements[Number(child.getAttribute(ID))];
          if (element && ownElements.has(element)) {
            order.push({element, model: child});
            ownElements.delete(element);
          }
        }
      }
      // Elements the engine dropped stay, at the end.
      for (const element of ownElements) {
        order.push({element, model: null});
      }
      if (!state.originalChildren.has(live)) {
        state.originalChildren.set(live, [...live.childNodes]);
        state.touched.push(live);
      }
      const children = [];
      let textIndex = 0;
      for (const part of order) {
        if ('text' in part) {
          let node = ownTexts[textIndex++];
          if (node) {
            writeText(node, part.text);
          } else {
            node = document.createTextNode('');
            state.created.add(node);
            node.data = part.text;
            state.written.set(node, part.text);
          }
          children.push(node);
        } else {
          children.push(part.element);
          if (part.model) {
            rebuild(part.element, part.model);
          }
        }
      }
      // Text nodes the translation had no place for: emptied, kept for
      // restore().
      for (const node of ownTexts.slice(textIndex)) {
        writeText(node, '');
      }
      // Comments and other nodes stay, after the content.
      for (const node of live.childNodes) {
        if (node.nodeType !== Node.TEXT_NODE &&
            node.nodeType !== Node.ELEMENT_NODE) {
          children.push(node);
        }
      }
      const current = [...live.childNodes];
      if (children.length !== current.length ||
          children.some((node, i) => node !== current[i])) {
        live.replaceChildren(...children);
      }
    };
    rebuild(unit.element, translated);
  }

  function cache() {
    const key = `${state.source}>${state.target}`;
    if (!caches.has(key)) {
      caches.set(key, new Map());
    }
    return caches.get(key);
  }

  function cacheKey(segment) {
    return (segment.html ? 'h' : 't') + segment.text;
  }

  // What a unit is, once queued: its text node, its element, or its
  // element's attribute.
  function keyOf(unit) {
    switch (unit.kind) {
      case 'node':
        return unit.node;
      case 'attribute':
        return `${unit.name}`;  // With the element, below.
      default:
        return unit.element;
    }
  }

  // Units in view go first; the rest follow in the page's order.
  function enqueue(units) {
    for (const unit of units) {
      unit.target = unit.kind === 'node' ? unit.node.parentElement :
                                           unit.element;
      if (!unit.target) {
        continue;
      }
      let queued;
      if (unit.kind === 'attribute') {
        queued = state.queuedAttributes.get(unit.element);
        if (!queued) {
          queued = new Set();
          state.queuedAttributes.set(unit.element, queued);
        }
      } else {
        queued = state.queued;
      }
      const key = keyOf(unit);
      if (queued.has(key)) {
        continue;
      }
      queued.add(key);
      unit.release = () => queued.delete(key);
      state.waiting.push(unit);
      state.intersection?.observe(unit.target);
    }
    schedule();
  }

  function schedule() {
    if (state.scheduled) {
      return;
    }
    state.scheduled = true;
    queueMicrotask(() => {
      if (!state) {
        return;
      }
      state.scheduled = false;
      pump();
    });
  }

  function takeBatch() {
    const batch = [];
    let chars = 0;
    const take = predicate => {
      for (let i = 0; i < state.waiting.length;) {
        if (batch.length >= MAX_BATCH_SEGMENTS || chars >= MAX_BATCH_CHARS) {
          return;
        }
        const unit = state.waiting[i];
        if (!predicate(unit)) {
          i++;
          continue;
        }
        state.waiting.splice(i, 1);
        if (!unit.target?.isConnected && unit.kind !== 'node') {
          continue;
        }
        if (unit.kind === 'node' && !unit.node.isConnected) {
          continue;
        }
        unit.segment = segmentOf(unit);
        chars += unit.segment.text.length;
        batch.push(unit);
      }
    };
    take(unit => state.visible.has(unit.target));
    take(() => true);
    return batch;
  }

  function pump() {
    while (state && state.inFlight < MAX_IN_FLIGHT && state.waiting.length) {
      const batch = takeBatch();
      if (!batch.length) {
        break;
      }
      // From the cache, at once.
      const known = cache();
      const missing = [];
      for (const unit of batch) {
        const translation = known.get(cacheKey(unit.segment));
        if (translation !== undefined) {
          write(() => apply(unit, translation));
          unit.release();
          state.done++;
        } else {
          missing.push(unit);
        }
      }
      if (!missing.length) {
        continue;
      }
      state.inFlight++;
      const generation = state.generation;
      const current = state;
      Promise.resolve()
          .then(() => current.engine(missing.map(unit => unit.segment)))
          .then(
              translations => {
                if (state !== current || generation !== current.generation) {
                  return;
                }
                missing.forEach((unit, index) => {
                  const translation = translations?.[index];
                  if (typeof translation !== 'string') {
                    state.failed++;
                    return;
                  }
                  known.set(cacheKey(unit.segment), translation);
                  // The page changed it meanwhile: send it again.
                  if (cacheKey(segmentOf(unit)) !== cacheKey(unit.segment)) {
                    enqueue([unit]);
                    return;
                  }
                  write(() => apply(unit, translation));
                  state.done++;
                });
              },
              () => {
                if (state === current) {
                  state.failed += missing.length;
                }
              })
          .finally(() => {
            current.inFlight--;
            if (state === current) {
              missing.forEach(unit => unit.release());
              firstBack();
              pump();
              settle();
            }
          });
    }
    settle();
  }

  function settle() {
    if (state && !state.waiting.length && !state.inFlight) {
      firstBack();
      const waiters = state.idleWaiters;
      state.idleWaiters = [];
      waiters.forEach(resolve => resolve());
    }
  }

  function firstBack() {
    if (!state || state.firstBack) {
      return;
    }
    state.firstBack = true;
    const waiters = state.firstWaiters;
    state.firstWaiters = [];
    waiters.forEach(resolve => resolve());
  }

  // Our own writes aren't the page changing: their mutation records are
  // dropped as soon as they're made.
  function write(change) {
    change();
    state.mutations?.takeRecords();
  }

  function onMutations(records) {
    if (!state) {
      return;
    }
    const units = [];
    for (const record of records) {
      if (record.type === 'characterData') {
        const node = record.target;
        if (state.written.get(node) === node.data) {
          continue;
        }
        // The page wrote over a translation: that's its text now, sent on
        // its own.
        state.originalText.delete(node);
        state.written.delete(node);
        collectUnits(node, units);
      } else if (record.type === 'childList') {
        for (const node of record.addedNodes) {
          if (!state.created.has(node)) {
            collectUnits(node, units);
          }
        }
      } else if (record.type === 'attributes') {
        const value = record.target.getAttribute(record.attributeName);
        if (value && worthTranslating(value)) {
          units.push({
            kind: 'attribute',
            element: record.target,
            name: record.attributeName,
          });
          state.originalAttributes.get(record.target)
              ?.set(record.attributeName, value);
        }
      }
    }
    if (units.length) {
      enqueue(units);
    }
  }

  function start({source, target, engine}) {
    // Into another language (Translate To): from the original, not from
    // the last translation.
    restore();
    state = {
      source, target, engine,
      waiting: [],
      queued: new Set(),
      queuedAttributes: new WeakMap(),
      visible: new Set(),
      inFlight: 0,
      done: 0,
      failed: 0,
      generation: 1,
      scheduled: false,
      idleWaiters: [],
      firstWaiters: [],
      firstBack: false,
      originalText: new WeakMap(),
      originalAttributes: new WeakMap(),
      originalChildren: new WeakMap(),
      written: new WeakMap(),
      created: new WeakSet(),
      touched: [],
      intersection: null,
      mutations: null,
    };
    if (typeof IntersectionObserver === 'function') {
      state.intersection = new IntersectionObserver(entries => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            state?.visible.add(entry.target);
          } else {
            state?.visible.delete(entry.target);
          }
        }
        if (state) {
          pump();
        }
      }, {rootMargin: '50% 0px'});
    }
    state.mutations = new MutationObserver(onMutations);
    state.mutations.observe(document.documentElement, {
      subtree: true,
      childList: true,
      characterData: true,
      attributes: true,
      attributeFilter: ATTRIBUTES,
    });
    const units = [];
    collectUnits(document.body || document.documentElement, units);
    if (document.title && worthTranslating(document.title)) {
      const title = document.querySelector('title');
      if (title) {
        units.push({kind: 'text', element: title});
      }
    }
    enqueue(units);
  }

  // Stops, keeping what's translated and what restore() needs.
  function stop() {
    if (!state) {
      return;
    }
    state.generation++;
    state.mutations?.disconnect();
    state.intersection?.disconnect();
    state.waiting = [];
    firstBack();
    const waiters = state.idleWaiters;
    state.idleWaiters = [];
    waiters.forEach(resolve => resolve());
    state.stopped = true;
  }

  // Back to the page as it was, except what the page itself changed since.
  function restore() {
    if (!state) {
      return;
    }
    stop();
    const current = state;
    for (const node of current.touched) {
      if (node.nodeType === Node.TEXT_NODE) {
        if (current.originalText.has(node) &&
            current.written.get(node) === node.data) {
          node.data = current.originalText.get(node);
        }
        continue;
      }
      const children = current.originalChildren.get(node);
      if (children) {
        node.replaceChildren(...children);
      }
      const attributes = current.originalAttributes.get(node);
      if (attributes) {
        for (const [name, value] of attributes) {
          if (value === null) {
            node.removeAttribute(name);
          } else if (name === 'value' && node.tagName === 'INPUT') {
            node.value = value;
          } else {
            node.setAttribute(name, value);
          }
        }
      }
    }
    state = null;
  }

  function progress() {
    if (!state) {
      return {done: 0, pending: 0, failed: 0};
    }
    return {
      done: state.done,
      pending: state.waiting.length + state.inFlight,
      failed: state.failed,
    };
  }

  function idle() {
    if (!state || state.stopped ||
        (!state.waiting.length && !state.inFlight)) {
      return Promise.resolve();
    }
    return new Promise(resolve => state.idleWaiters.push(resolve));
  }

  function started() {
    if (!state || state.stopped || state.firstBack) {
      return Promise.resolve();
    }
    return new Promise(resolve => state.firstWaiters.push(resolve));
  }

  globalThis.__compassTranslator =
      {start, stop, restore, progress, idle, started};
})();
