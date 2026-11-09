// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Text helpers shared by the live folder SDK: entity decoding, tag
// stripping, attribute parsing and feed date parsing. A classic script, so
// every runtime (a Worker on desktop and Android, JavaScriptCore on iOS, a
// vm context in tools/live_folders) can load it without a module loader.
// Plugins reach it as `compass.text`. See docs/design/live-folders.md.

(function (root) {
  'use strict';

  const NAMED = {
    amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ',
    hellip: '…', mdash: '—', ndash: '–', lsquo: '‘',
    rsquo: '’', sbquo: '‚', ldquo: '“', rdquo: '”',
    bdquo: '„', laquo: '«', raquo: '»', copy: '©',
    reg: '®', trade: '™', euro: '€', pound: '£',
    yen: '¥', cent: '¢', middot: '·', bull: '•',
    deg: '°', times: '×', divide: '÷', sect: '§',
    para: '¶', shy: '­', zwj: '‍', zwnj: '‌',
    thinsp: ' ', ensp: ' ', emsp: ' ', iexcl: '¡',
    iquest: '¿', larr: '←', rarr: '→', uarr: '↑',
    darr: '↓', harr: '↔', check: '✓',
    agrave: 'à', aacute: 'á', acirc: 'â', atilde: 'ã',
    auml: 'ä', aring: 'å', aelig: 'æ', ccedil: 'ç',
    egrave: 'è', eacute: 'é', ecirc: 'ê', euml: 'ë',
    igrave: 'ì', iacute: 'í', icirc: 'î', iuml: 'ï',
    ntilde: 'ñ', ograve: 'ò', oacute: 'ó', ocirc: 'ô',
    otilde: 'õ', ouml: 'ö', oslash: 'ø', ugrave: 'ù',
    uacute: 'ú', ucirc: 'û', uuml: 'ü', yacute: 'ý',
    yuml: 'ÿ', szlig: 'ß',
    Agrave: 'À', Aacute: 'Á', Acirc: 'Â', Atilde: 'Ã',
    Auml: 'Ä', Aring: 'Å', AElig: 'Æ', Ccedil: 'Ç',
    Egrave: 'È', Eacute: 'É', Ecirc: 'Ê', Euml: 'Ë',
    Igrave: 'Ì', Iacute: 'Í', Icirc: 'Î', Iuml: 'Ï',
    Ntilde: 'Ñ', Ograve: 'Ò', Oacute: 'Ó', Ocirc: 'Ô',
    Otilde: 'Õ', Ouml: 'Ö', Oslash: 'Ø', Ugrave: 'Ù',
    Uacute: 'Ú', Ucirc: 'Û', Uuml: 'Ü', Yacute: 'Ý',
  };

  function codePoint(n) {
    if (!(n > 0 && n <= 0x10ffff) || (n >= 0xd800 && n <= 0xdfff)) {
      return '�';
    }
    return String.fromCodePoint(n);
  }

  // Decodes named (a common subset of HTML's), decimal and hex character
  // references. Unknown names are left as written.
  function decodeEntities(s) {
    if (typeof s !== 'string' || s.indexOf('&') < 0) return s == null ? '' : String(s);
    return s.replace(/&(#[xX][0-9a-fA-F]+|#[0-9]+|[A-Za-z][A-Za-z0-9]*);/g,
        (whole, ref) => {
          if (ref[0] === '#') {
            const hex = ref[1] === 'x' || ref[1] === 'X';
            return codePoint(parseInt(ref.slice(hex ? 2 : 1), hex ? 16 : 10));
          }
          return Object.prototype.hasOwnProperty.call(NAMED, ref) ? NAMED[ref] : whole;
        });
  }

  function collapse(s) {
    return String(s == null ? '' : s).replace(/[\s ]+/g, ' ').trim();
  }

  // Markup to plain text: drops comments, scripts, styles and tags, decodes
  // entities and collapses whitespace. Good enough for titles and summaries;
  // not an HTML parser.
  function stripTags(html) {
    if (html == null) return '';
    const s = String(html)
        .replace(/<!--[\s\S]*?-->/g, ' ')
        .replace(/<(script|style|noscript|template)\b[\s\S]*?<\/\1\s*>/gi, ' ')
        .replace(/<br\s*\/?>/gi, ' ')
        .replace(/<\/?[A-Za-z][^>]*>/g, ' ');
    return collapse(decodeEntities(s));
  }

  // `a="1" b='2' c=3 d` → {a: '1', b: '2', c: '3', d: ''}. Names are lower
  // cased when `lowerNames` is set (HTML), kept as written otherwise (XML).
  function parseAttributes(s, lowerNames) {
    const attrs = {};
    const re = /([^\s"'<>\/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?/g;
    let m;
    while ((m = re.exec(s)) !== null) {
      const name = lowerNames ? m[1].toLowerCase() : m[1];
      if (Object.prototype.hasOwnProperty.call(attrs, name)) continue;
      const raw = m[2] !== undefined ? m[2] : m[3] !== undefined ? m[3] : m[4];
      attrs[name] = raw === undefined ? '' : decodeEntities(raw);
    }
    return attrs;
  }

  const MONTHS = {jan: 0, feb: 1, mar: 2, apr: 3, may: 4, jun: 5, jul: 6,
                  aug: 7, sep: 8, oct: 9, nov: 10, dec: 11};
  const ZONES = {UT: 0, UTC: 0, GMT: 0, Z: 0, EST: -300, EDT: -240,
                 CST: -360, CDT: -300, MST: -420, MDT: -360, PST: -480,
                 PDT: -420};

  function zoneMinutes(z) {
    if (!z) return 0;
    const u = z.toUpperCase();
    if (Object.prototype.hasOwnProperty.call(ZONES, u)) return ZONES[u];
    const m = /^([+-])(\d{2}):?(\d{2})?$/.exec(z);
    if (!m) return null;
    const v = parseInt(m[2], 10) * 60 + parseInt(m[3] || '0', 10);
    return m[1] === '-' ? -v : v;
  }

  // Feed dates: ISO 8601 / RFC 3339 (Atom, JSON Feed) and RFC 822 / 2822
  // (RSS). Returns epoch milliseconds, or null. A date without a zone is
  // read as UTC, so results don't depend on where the plugin runs.
  function parseDate(input) {
    if (input == null) return null;
    if (typeof input === 'number') return isFinite(input) ? input : null;
    const s = collapse(input);
    let m = /^(\d{4})-(\d{2})-(\d{2})(?:[T ](\d{2}):(\d{2})(?::(\d{2})(?:[.,](\d+))?)?\s*(Z|[+-]\d{2}:?\d{2})?)?$/i.exec(s);
    if (m) {
      const ms = m[7] ? Math.round(parseFloat('0.' + m[7]) * 1000) : 0;
      const t = Date.UTC(+m[1], +m[2] - 1, +m[3], +(m[4] || 0), +(m[5] || 0),
                         +(m[6] || 0), ms);
      const off = zoneMinutes(m[8]);
      return off === null || isNaN(t) ? null : t - off * 60000;
    }
    m = /^(?:[A-Za-z]{3,},?\s+)?(\d{1,2})\s+([A-Za-z]{3})[A-Za-z]*\.?\s+(\d{2,4})(?:\s+(\d{1,2}):(\d{2})(?::(\d{2}))?)?(?:\s*([A-Za-z]{1,5}|[+-]\d{2}:?\d{2}))?$/.exec(s);
    if (m) {
      const month = MONTHS[m[2].toLowerCase()];
      if (month === undefined) return null;
      let year = parseInt(m[3], 10);
      if (m[3].length === 2) year += year < 50 ? 2000 : 1900;
      const t = Date.UTC(year, month, +m[1], +(m[4] || 0), +(m[5] || 0), +(m[6] || 0));
      const off = zoneMinutes(m[7]);
      return off === null || isNaN(t) ? null : t - off * 60000;
    }
    return null;
  }

  const api = Object.freeze({decodeEntities, stripTags, collapse,
                             parseAttributes, parseDate});
  root.CompassSDK = root.CompassSDK || {};
  root.CompassSDK.text = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
