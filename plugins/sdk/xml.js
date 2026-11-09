// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A small, lenient, non-validating XML parser for feeds. No runtime gives
// plugins a DOMParser (Workers and JavaScriptCore have none), so the SDK
// brings its own. Plugins reach it as `compass.xml`:
//
//   const doc = compass.xml.parse(text);
//   for (const item of doc.findAll('item')) item.childText('title');
//
// Element names keep their prefix (`content:encoded`); lookups by a name
// without a prefix also match prefixed elements with that local name.
// Malformed input never throws: unclosed elements close at the end, stray
// end tags are ignored.

(function (root) {
  'use strict';

  const text = (root.CompassSDK && root.CompassSDK.text) ||
      (typeof require === 'function' ? require('./text.js') : null);

  function localName(name) {
    const i = name.indexOf(':');
    return i < 0 ? name : name.slice(i + 1);
  }

  function matches(el, name) {
    if (name === '*') return true;
    return el.name === name || (name.indexOf(':') < 0 && localName(el.name) === name);
  }

  class Element {
    constructor(name, attrs) {
      this.name = name;
      this.attrs = attrs;
      this.children = [];  // Elements and strings, in document order.
    }

    get localName() { return localName(this.name); }

    attr(name) {
      if (Object.prototype.hasOwnProperty.call(this.attrs, name)) return this.attrs[name];
      for (const k of Object.keys(this.attrs)) {
        if (localName(k) === name) return this.attrs[k];
      }
      return null;
    }

    // Direct child elements, optionally only those named `name`.
    elements(name) {
      return this.children.filter(
          (c) => c instanceof Element && (name === undefined || matches(c, name)));
    }

    // First child named `name`. An exact name wins over a prefixed one, so
    // child('link') in an RSS item finds <link>, not <atom:link>.
    child(name) {
      let prefixed = null;
      for (const c of this.children) {
        if (!(c instanceof Element)) continue;
        if (c.name === name || name === '*') return c;
        if (!prefixed && matches(c, name)) prefixed = c;
      }
      return prefixed;
    }

    // First descendant named `name`, depth first.
    find(name) {
      for (const c of this.children) {
        if (!(c instanceof Element)) continue;
        if (matches(c, name)) return c;
        const f = c.find(name);
        if (f) return f;
      }
      return null;
    }

    findAll(name, out) {
      out = out || [];
      for (const c of this.children) {
        if (!(c instanceof Element)) continue;
        if (matches(c, name)) out.push(c);
        c.findAll(name, out);
      }
      return out;
    }

    // Raw text content (entities decoded, CDATA kept as is), untrimmed.
    rawText() {
      let s = '';
      for (const c of this.children) s += typeof c === 'string' ? c : c.rawText();
      return s;
    }

    // Text content with whitespace collapsed.
    text() { return text.collapse(this.rawText()); }

    // Text of the first child named `name`, or '' when there's none.
    childText(name) {
      const c = this.child(name);
      return c ? c.text() : '';
    }
  }

  function parse(src) {
    src = String(src == null ? '' : src);
    if (src.charCodeAt(0) === 0xfeff) src = src.slice(1);
    const doc = new Element('#document', {});
    const stack = [doc];
    const top = () => stack[stack.length - 1];
    const n = src.length;
    let i = 0;

    while (i < n) {
      const lt = src.indexOf('<', i);
      if (lt < 0) {
        top().children.push(text.decodeEntities(src.slice(i)));
        break;
      }
      if (lt > i) top().children.push(text.decodeEntities(src.slice(i, lt)));
      i = lt;

      if (src.startsWith('<!--', i)) {
        const end = src.indexOf('-->', i + 4);
        i = end < 0 ? n : end + 3;
      } else if (src.startsWith('<![CDATA[', i)) {
        const end = src.indexOf(']]>', i + 9);
        top().children.push(src.slice(i + 9, end < 0 ? n : end));
        i = end < 0 ? n : end + 3;
      } else if (src.startsWith('<?', i)) {
        const end = src.indexOf('?>', i + 2);
        i = end < 0 ? n : end + 2;
      } else if (src.startsWith('<!', i)) {
        // DOCTYPE, possibly with an internal subset in brackets.
        let j = i + 2;
        let depth = 0;
        while (j < n) {
          const c = src[j];
          if (c === '[') depth++;
          else if (c === ']') depth--;
          else if (c === '>' && depth <= 0) break;
          j++;
        }
        i = j + 1;
      } else if (src.startsWith('</', i)) {
        const end = src.indexOf('>', i + 2);
        const name = src.slice(i + 2, end < 0 ? n : end).trim();
        i = end < 0 ? n : end + 1;
        for (let k = stack.length - 1; k > 0; k--) {
          if (stack[k].name === name) {
            stack.length = k;
            break;
          }
        }
      } else {
        // Start tag: find its end, skipping quoted attribute values.
        let j = i + 1;
        let quote = null;
        while (j < n) {
          const c = src[j];
          if (quote) {
            if (c === quote) quote = null;
          } else if (c === '"' || c === "'") {
            quote = c;
          } else if (c === '>') {
            break;
          }
          j++;
        }
        let body = src.slice(i + 1, j);
        i = j + 1;
        const selfClosing = /\/\s*$/.test(body);
        if (selfClosing) body = body.replace(/\/\s*$/, '');
        const m = /^([^\s\/>]+)\s*([\s\S]*)$/.exec(body);
        if (!m) {
          top().children.push('<');  // A lone '<' in text.
          i = lt + 1;
          continue;
        }
        const el = new Element(m[1], text.parseAttributes(m[2], false));
        top().children.push(el);
        if (!selfClosing) stack.push(el);
      }
    }
    return doc;
  }

  const api = Object.freeze({parse, Element});
  root.CompassSDK = root.CompassSDK || {};
  root.CompassSDK.xml = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
