// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The web globals a bare JSContext lacks (URL, URLSearchParams, TextEncoder,
// TextDecoder), for the iOS live folder runtime (docs/design/live-folders.md,
// "Runtime"). Loaded first into each plugin context and into the host's own
// context. Ports of the WHATWG URL Standard (https://url.spec.whatwg.org/) and
// Encoding Standard (https://encoding.spec.whatwg.org/); comments name their
// sections. Uses no eval or Function: the context disables them afterwards.

(function (global) {
  'use strict';

  // ---------------------------------------------------------------------
  // Shared helpers

  // WebIDL USVString: ToString (throws on symbols), lone surrogates -> U+FFFD.
  function usv(value) {
    const s = `${value}`;
    if (!/[\uD800-\uDFFF]/.test(s)) return s;
    let out = '';
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff) {
        const d = s.charCodeAt(i + 1);
        if (d >= 0xdc00 && d <= 0xdfff) { out += s[i] + s[++i]; continue; }
      }
      out += c >= 0xd800 && c <= 0xdfff ? '\uFFFD' : s[i];
    }
    return out;
  }

  function utf8Encode(s) {
    const out = new Uint8Array(s.length * 3);
    let n = 0;
    for (let i = 0; i < s.length; i++) {
      let c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdfff) {
        const d = s.charCodeAt(i + 1);
        if (c <= 0xdbff && d >= 0xdc00 && d <= 0xdfff) {
          c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
          i++;
        } else {
          c = 0xfffd;
        }
      }
      if (c < 0x80) {
        out[n++] = c;
      } else if (c < 0x800) {
        out[n++] = 0xc0 | (c >> 6);
        out[n++] = 0x80 | (c & 0x3f);
      } else if (c < 0x10000) {
        out[n++] = 0xe0 | (c >> 12);
        out[n++] = 0x80 | ((c >> 6) & 0x3f);
        out[n++] = 0x80 | (c & 0x3f);
      } else {
        out[n++] = 0xf0 | (c >> 18);
        out[n++] = 0x80 | ((c >> 12) & 0x3f);
        out[n++] = 0x80 | ((c >> 6) & 0x3f);
        out[n++] = 0x80 | (c & 0x3f);
      }
    }
    return out.slice(0, n);
  }

  function pushCodePoint(units, cp) {
    if (cp < 0x10000) {
      units.push(cp);
    } else {
      cp -= 0x10000;
      units.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
    }
  }

  function unitsToString(units, start) {
    let s = '';
    for (let i = start; i < units.length; i += 8192) {
      s += String.fromCharCode.apply(null, units.slice(i, i + 8192));
    }
    return s;
  }

  // Encoding Standard decoders. Each returns a function that decodes a chunk
  // of bytes into `units` (UTF-16 code units), keeping state across chunks;
  // `flush` ends the stream and `fail(units)` handles an error.

  // "UTF-8 decoder": maximal subparts become one U+FFFD each.
  function utf8Decoder() {
    let needed = 0, seen = 0, cp = 0, lower = 0x80, upper = 0xbf;
    return function (bytes, flush, units, fail) {
      for (let i = 0; i < bytes.length; i++) {
        const b = bytes[i];
        if (needed === 0) {
          if (b < 0x80) {
            units.push(b);
          } else if (b >= 0xc2 && b <= 0xdf) {
            needed = 1; cp = b & 0x1f;
          } else if (b >= 0xe0 && b <= 0xef) {
            if (b === 0xe0) lower = 0xa0;
            if (b === 0xed) upper = 0x9f;
            needed = 2; cp = b & 0xf;
          } else if (b >= 0xf0 && b <= 0xf4) {
            if (b === 0xf0) lower = 0x90;
            if (b === 0xf4) upper = 0x8f;
            needed = 3; cp = b & 0x7;
          } else {
            fail(units);
          }
          continue;
        }
        if (b < lower || b > upper) {
          // Error, then reprocess this byte as a new sequence.
          needed = seen = cp = 0; lower = 0x80; upper = 0xbf;
          i--;
          fail(units);
          continue;
        }
        lower = 0x80; upper = 0xbf;
        cp = (cp << 6) | (b & 0x3f);
        if (++seen === needed) {
          pushCodePoint(units, cp);
          needed = seen = cp = 0;
        }
      }
      if (flush && needed) {
        needed = seen = cp = 0; lower = 0x80; upper = 0xbf;
        fail(units);
      }
    };
  }

  // "Shared UTF-16 decoder".
  function utf16Decoder(bigEndian) {
    let lead = -1, high = -1;
    return function (bytes, flush, units, fail) {
      for (let i = 0; i < bytes.length; i++) {
        if (lead < 0) { lead = bytes[i]; continue; }
        const cu = bigEndian ? (lead << 8) | bytes[i] : (bytes[i] << 8) | lead;
        lead = -1;
        if (high >= 0) {
          const h = high;
          high = -1;
          if (cu >= 0xdc00 && cu <= 0xdfff) { units.push(h, cu); continue; }
          fail(units);
        }
        if (cu >= 0xd800 && cu <= 0xdbff) high = cu;
        else if (cu >= 0xdc00 && cu <= 0xdfff) fail(units);
        else units.push(cu);
      }
      if (flush && (lead >= 0 || high >= 0)) {
        lead = high = -1;
        fail(units);
      }
    };
  }

  // "Single-byte decoder" for windows-1252 (also latin1 and ascii labels):
  // index-windows-1252 differs from Latin-1 in 0x80-0x9F.
  const W1252 =
    '\u20AC\x81\u201A\u0192\u201E\u2026\u2020\u2021\u02C6\u2030\u0160\u2039' +
    '\u0152\x8D\u017D\x8F\x90\u2018\u2019\u201C\u201D\u2022\u2013\u2014' +
    '\u02DC\u2122\u0161\u203A\u0153\x9D\u017E\u0178';
  function windows1252Decoder() {
    return function (bytes, flush, units) {
      for (let i = 0; i < bytes.length; i++) {
        const b = bytes[i];
        units.push(b >= 0x80 && b < 0xa0 ? W1252.charCodeAt(b - 0x80) : b);
      }
    };
  }

  const replace = (units) => units.push(0xfffd);

  // "UTF-8 decode without BOM", as the URL Standard uses it.
  function utf8DecodeNoBOM(bytes) {
    const units = [];
    utf8Decoder()(bytes, true, units, replace);
    return unitsToString(units, 0);
  }

  // ---------------------------------------------------------------------
  // URL Standard: percent-encoding (section 1.3)

  const hex2 = (b) => '%' + (b < 16 ? '0' : '') + b.toString(16).toUpperCase();
  const isHexByte = (b) =>
    (b >= 0x30 && b <= 0x39) || (b >= 0x41 && b <= 0x46) || (b >= 0x61 && b <= 0x66);

  const inC0 = (c) => c < 0x20 || c > 0x7e;
  const extend = (set, chars) => (c) => set(c) || (c < 0x80 && chars.includes(String.fromCharCode(c)));
  const FRAGMENT_SET = extend(inC0, ' "<>`');
  const QUERY_SET = extend(inC0, ' "#<>');
  const SPECIAL_QUERY_SET = extend(QUERY_SET, "'");
  const PATH_SET = extend(QUERY_SET, '?^`{}');
  const USERINFO_SET = extend(PATH_SET, '/:;=@[\\]|');

  function percentEncode(s, set) {
    let out = '';
    for (const ch of s) {
      if (!set(ch.codePointAt(0))) { out += ch; continue; }
      for (const b of utf8Encode(ch)) out += hex2(b);
    }
    return out;
  }

  function percentDecode(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      if (b === 0x25 && i + 2 < bytes.length && isHexByte(bytes[i + 1]) && isHexByte(bytes[i + 2])) {
        out.push(parseInt(String.fromCharCode(bytes[i + 1], bytes[i + 2]), 16));
        i += 2;
      } else {
        out.push(b);
      }
    }
    return out;
  }

  // ---------------------------------------------------------------------
  // URL Standard: hosts (section 3)

  // Punycode (RFC 3492 section 6) for IDNA labels.
  const PUNY = { base: 36, tmin: 1, tmax: 26, skew: 38, damp: 700 };
  function adapt(delta, numPoints, first) {
    delta = first ? Math.floor(delta / PUNY.damp) : delta >> 1;
    delta += Math.floor(delta / numPoints);
    let k = 0;
    for (; delta > ((PUNY.base - PUNY.tmin) * PUNY.tmax) >> 1; k += PUNY.base) {
      delta = Math.floor(delta / (PUNY.base - PUNY.tmin));
    }
    return Math.floor(k + ((PUNY.base - PUNY.tmin + 1) * delta) / (delta + PUNY.skew));
  }
  const threshold = (k, bias) =>
    k <= bias ? PUNY.tmin : k >= bias + PUNY.tmax ? PUNY.tmax : k - bias;
  const digitChar = (d) => String.fromCharCode(d < 26 ? 0x61 + d : 0x16 + d);

  function punycodeEncode(label) {
    const cps = Array.from(label, (ch) => ch.codePointAt(0));
    let out = cps.filter((c) => c < 0x80).map((c) => String.fromCharCode(c)).join('');
    let n = 0x80, delta = 0, bias = 72, h = out.length;
    const b = h;
    if (b > 0) out += '-';
    while (h < cps.length) {
      const m = Math.min(...cps.filter((c) => c >= n));
      delta += (m - n) * (h + 1);
      n = m;
      for (const c of cps) {
        if (c < n) delta++;
        if (c !== n) continue;
        let q = delta;
        for (let k = PUNY.base; ; k += PUNY.base) {
          const t = threshold(k, bias);
          if (q < t) break;
          out += digitChar(t + ((q - t) % (PUNY.base - t)));
          q = Math.floor((q - t) / (PUNY.base - t));
        }
        out += digitChar(q);
        bias = adapt(delta, h + 1, h === b);
        delta = 0;
        h++;
      }
      delta++;
      n++;
    }
    return out;
  }

  // Returns the decoded label, or null on failure.
  function punycodeDecode(input) {
    const out = [];
    let basic = input.lastIndexOf('-');
    if (basic < 0) basic = 0;
    for (let j = 0; j < basic; j++) {
      if (input.charCodeAt(j) >= 0x80) return null;
      out.push(input.charCodeAt(j));
    }
    let n = 0x80, i = 0, bias = 72;
    for (let idx = basic > 0 ? basic + 1 : 0; idx < input.length; ) {
      const oldi = i;
      for (let w = 1, k = PUNY.base; ; k += PUNY.base) {
        if (idx >= input.length) return null;
        const c = input.charCodeAt(idx++);
        const digit = c >= 0x30 && c <= 0x39 ? c - 0x16 : c >= 0x61 && c <= 0x7a ? c - 0x61
          : c >= 0x41 && c <= 0x5a ? c - 0x41 : PUNY.base;
        if (digit >= PUNY.base || digit > Math.floor((0x7fffffff - i) / w)) return null;
        i += digit * w;
        const t = threshold(k, bias);
        if (digit < t) break;
        w *= PUNY.base - t;
      }
      bias = adapt(i - oldi, out.length + 1, oldi === 0);
      n += Math.floor(i / (out.length + 1));
      if (n > 0x10ffff) return null;
      i %= out.length + 1;
      out.splice(i++, 0, n);
    }
    return String.fromCodePoint(...out);
  }

  // Code points UTS #46 ignores, and an approximation of the ones it
  // disallows (controls, unassigned, private use, U+FFFD...).
  const IDNA_IGNORED = /[\u00AD\u034F\u180B-\u180F\u200B\u2060-\u2064\uFE00-\uFE0F\uFEFF\u{E0100}-\u{E01EF}]/gu;
  const IDNA_DISALLOWED = /[\p{Cc}\p{Cn}\p{Co}\p{Cs}\uFFFD]/u;

  // "domain to ASCII" (3.5) with be strict false. ASCII input is only
  // lowercased, as in browsers and Node. For other input, NFKC plus
  // toLowerCase() stands in for the UTS #46 mapping table (they agree for
  // most text), then labels are checked and Punycode-encoded.
  function domainToASCII(domain) {
    if (!/[^\x00-\x7f]/.test(domain)) return domain === '' ? null : domain.toLowerCase();
    // UTS #46 folds every capital sigma to U+03C3, without final-sigma context.
    let s = domain.replace(IDNA_IGNORED, '').normalize('NFKC').replace(/\u03A3/g, '\u03C3');
    s = s.toLowerCase().normalize('NFC');
    s = s.replace(/[\u3002\uFF0E\uFF61]/g, '.');
    if (IDNA_DISALLOWED.test(s)) return null;
    const labels = s.split('.').map((label) => {
      if (label.startsWith('xn--')) {
        const decoded = punycodeDecode(label.slice(4));
        if (!decoded || !/[^\x00-\x7f]/.test(decoded) || decoded !== decoded.normalize('NFC') ||
            IDNA_DISALLOWED.test(decoded) || decoded.toLowerCase() !== decoded) {
          return null;
        }
        return label;
      }
      return /[^\x00-\x7f]/.test(label) ? 'xn--' + punycodeEncode(label) : label;
    });
    if (labels.includes(null)) return null;
    const result = labels.join('.');
    return result === '' ? null : result;
  }

  const FORBIDDEN_HOST = /[\x00\t\n\r #/:<>?@[\\\]^|]/;
  const FORBIDDEN_DOMAIN = /[\x00-\x20#%/:<>?@[\\\]^|\x7f]/;

  // "host parser" (3.5). Hosts are kept serialized; null means failure.
  function parseHost(input, isOpaque) {
    if (input[0] === '[') {
      if (input[input.length - 1] !== ']') return null;
      const address = parseIPv6(input.slice(1, -1));
      return address && '[' + serializeIPv6(address) + ']';
    }
    if (isOpaque) {
      // "opaque-host parser"
      if (FORBIDDEN_HOST.test(input)) return null;
      return percentEncode(input, inC0);
    }
    const domain = utf8DecodeNoBOM(percentDecode(utf8Encode(input)));
    const ascii = domainToASCII(domain);
    if (ascii === null || FORBIDDEN_DOMAIN.test(ascii)) return null;
    if (endsInANumber(ascii)) {
      const ipv4 = parseIPv4(ascii);
      return ipv4 === null ? null : [24, 16, 8, 0].map((s) => (ipv4 >>> s) & 255).join('.');
    }
    return ascii;
  }

  // "ends in a number checker"
  function endsInANumber(input) {
    const parts = input.split('.');
    if (parts[parts.length - 1] === '') {
      if (parts.length === 1) return false;
      parts.pop();
    }
    const last = parts[parts.length - 1];
    return /^[0-9]+$/.test(last) || parseIPv4Number(last) !== null;
  }

  // "IPv4 number parser": null on failure.
  function parseIPv4Number(input) {
    if (input === '') return null;
    let radix = 10;
    if (/^0[xX]/.test(input)) {
      input = input.slice(2);
      radix = 16;
    } else if (input.length >= 2 && input[0] === '0') {
      input = input.slice(1);
      radix = 8;
    }
    if (input === '') return 0;
    const valid = radix === 10 ? /^[0-9]+$/ : radix === 16 ? /^[0-9A-Fa-f]+$/ : /^[0-7]+$/;
    return valid.test(input) ? parseInt(input, radix) : null;
  }

  // "IPv4 parser": the address as a number, or null.
  function parseIPv4(input) {
    const parts = input.split('.');
    if (parts[parts.length - 1] === '' && parts.length > 1) parts.pop();
    if (parts.length > 4) return null;
    const numbers = [];
    for (const part of parts) {
      const n = parseIPv4Number(part);
      if (n === null) return null;
      numbers.push(n);
    }
    const last = numbers.pop();
    if (numbers.some((n) => n > 255) || last >= 256 ** (4 - numbers.length)) return null;
    return numbers.reduce((ipv4, n, i) => ipv4 + n * 256 ** (3 - i), last);
  }

  // "IPv6 parser": eight 16-bit pieces, or null.
  function parseIPv6(input) {
    const s = Array.from(input);
    const address = [0, 0, 0, 0, 0, 0, 0, 0];
    let pieceIndex = 0, compress = null, p = 0;
    const isHex = (c) => c !== undefined && /^[0-9A-Fa-f]$/.test(c);
    const isDigit = (c) => c !== undefined && /^[0-9]$/.test(c);
    if (s[p] === ':') {
      if (s[p + 1] !== ':') return null;
      p += 2;
      compress = ++pieceIndex;
    }
    while (p < s.length) {
      if (pieceIndex === 8) return null;
      if (s[p] === ':') {
        if (compress !== null) return null;
        p++;
        compress = ++pieceIndex;
        continue;
      }
      let value = 0, length = 0;
      while (length < 4 && isHex(s[p])) {
        value = value * 0x10 + parseInt(s[p], 16);
        p++;
        length++;
      }
      if (s[p] === '.') {
        // An embedded IPv4 address.
        if (length === 0) return null;
        p -= length;
        if (pieceIndex > 6) return null;
        let numbersSeen = 0;
        while (p < s.length) {
          let piece = null;
          if (numbersSeen > 0) {
            if (s[p] === '.' && numbersSeen < 4) p++;
            else return null;
          }
          if (!isDigit(s[p])) return null;
          while (isDigit(s[p])) {
            const n = +s[p];
            if (piece === null) piece = n;
            else if (piece === 0) return null;
            else piece = piece * 10 + n;
            if (piece > 255) return null;
            p++;
          }
          address[pieceIndex] = address[pieceIndex] * 0x100 + piece;
          numbersSeen++;
          if (numbersSeen === 2 || numbersSeen === 4) pieceIndex++;
        }
        if (numbersSeen !== 4) return null;
        break;
      } else if (s[p] === ':') {
        p++;
        if (p >= s.length) return null;
      } else if (p < s.length) {
        return null;
      }
      address[pieceIndex++] = value;
    }
    if (compress !== null) {
      let swaps = pieceIndex - compress;
      for (pieceIndex = 7; pieceIndex !== 0 && swaps > 0; pieceIndex--, swaps--) {
        const t = address[pieceIndex];
        address[pieceIndex] = address[compress + swaps - 1];
        address[compress + swaps - 1] = t;
      }
    } else if (pieceIndex !== 8) {
      return null;
    }
    return address;
  }

  // "host serializer" for IPv6: compress the first longest run of 2+ zeros.
  function serializeIPv6(address) {
    let compress = -1, best = 1;
    for (let i = 0; i < 8; ) {
      let j = i;
      while (j < 8 && address[j] === 0) j++;
      if (j - i > best) { best = j - i; compress = i; }
      i = j === i ? i + 1 : j;
    }
    let out = '';
    for (let i = 0; i < 8; i++) {
      if (i === compress) {
        out += i === 0 ? '::' : ':';
        i += best - 1;
        continue;
      }
      out += address[i].toString(16) + (i < 7 ? ':' : '');
    }
    return out;
  }

  // ---------------------------------------------------------------------
  // URL Standard: URLs (section 4)

  const SPECIAL = new Map([['ftp', 21], ['file', null], ['http', 80], ['https', 443], ['ws', 80], ['wss', 443]]);
  const isSpecial = (url) => SPECIAL.has(url.scheme);
  const hasOpaquePath = (url) => typeof url.path === 'string';
  const includesCredentials = (url) => url.username !== '' || url.password !== '';
  const cannotHaveCredentialsOrPort = (url) => url.host === null || url.host === '' || url.scheme === 'file';

  const isAlpha = (c) => /^[A-Za-z]$/.test(c);
  const isWindowsDriveLetter = (s) => /^[A-Za-z][:|]$/.test(s);
  const isNormalizedWindowsDriveLetter = (s) => /^[A-Za-z]:$/.test(s);
  // "starts with a Windows drive letter", on an array of code points.
  const startsWithWindowsDriveLetter = (cps, i) =>
    cps.length - i >= 2 && isWindowsDriveLetter(cps[i] + cps[i + 1]) &&
    (cps.length - i === 2 || '/\\?#'.includes(cps[i + 2]));
  const isSingleDot = (s) => s === '.' || s.toLowerCase() === '%2e';
  const isDoubleDot = (s) => /^(\.|%2e)(\.|%2e)$/i.test(s);

  // "shorten a URL's path"
  function shortenPath(url) {
    if (url.scheme === 'file' && url.path.length === 1 && isNormalizedWindowsDriveLetter(url.path[0])) return;
    url.path.pop();
  }

  // "potentially strip trailing spaces from an opaque path"
  function stripTrailingSpaces(url) {
    if (hasOpaquePath(url) && url.fragment === null && url.query === null) {
      url.path = url.path.replace(/ +$/, '');
    }
  }

  // "basic URL parser" (4.4). With `url` and `override` it modifies `url`
  // for the setters. Returns the URL record, or null on failure.
  function parseURL(input, base, url, override) {
    if (!url) {
      url = { scheme: '', username: '', password: '', host: null, port: null, path: [], query: null, fragment: null };
      input = input.replace(/^[\x00-\x20]+|[\x00-\x20]+$/g, '');
    }
    const s = Array.from(input.replace(/[\t\n\r]/g, ''));
    let state = override || 'scheme start';
    let buffer = '', atSignSeen = false, insideBrackets = false, passwordTokenSeen = false;

    for (let p = 0; p <= s.length; p++) {
      const c = p < s.length ? s[p] : ''; // '' is EOF
      switch (state) {
        case 'scheme start':
          if (isAlpha(c)) {
            buffer += c.toLowerCase();
            state = 'scheme';
          } else if (!override) {
            state = 'no scheme';
            p--;
          } else {
            return null;
          }
          break;

        case 'scheme':
          if (/^[A-Za-z0-9+\-.]$/.test(c)) {
            buffer += c.toLowerCase();
          } else if (c === ':') {
            if (override) {
              if (isSpecial(url) !== SPECIAL.has(buffer)) return url;
              if ((includesCredentials(url) || url.port !== null) && buffer === 'file') return url;
              if (url.scheme === 'file' && url.host === '') return url;
            }
            url.scheme = buffer;
            if (override) {
              if (url.port === SPECIAL.get(url.scheme)) url.port = null;
              return url;
            }
            buffer = '';
            if (url.scheme === 'file') {
              state = 'file';
            } else if (isSpecial(url) && base && base.scheme === url.scheme) {
              state = 'special relative or authority';
            } else if (isSpecial(url)) {
              state = 'special authority slashes';
            } else if (s[p + 1] === '/') {
              state = 'path or authority';
              p++;
            } else {
              url.path = '';
              state = 'opaque path';
            }
          } else if (!override) {
            buffer = '';
            state = 'no scheme';
            p = -1; // Start over from the first code point.
          } else {
            return null;
          }
          break;

        case 'no scheme':
          if (!base || (hasOpaquePath(base) && c !== '#')) {
            return null;
          } else if (hasOpaquePath(base) && c === '#') {
            url.scheme = base.scheme;
            url.path = base.path;
            url.query = base.query;
            url.fragment = '';
            state = 'fragment';
          } else {
            state = base.scheme !== 'file' ? 'relative' : 'file';
            p--;
          }
          break;

        case 'special relative or authority':
          if (c === '/' && s[p + 1] === '/') {
            state = 'special authority ignore slashes';
            p++;
          } else {
            state = 'relative';
            p--;
          }
          break;

        case 'path or authority':
          if (c === '/') {
            state = 'authority';
          } else {
            state = 'path';
            p--;
          }
          break;

        case 'relative':
          url.scheme = base.scheme;
          if (c === '/' || (isSpecial(url) && c === '\\')) {
            state = 'relative slash';
          } else {
            url.username = base.username;
            url.password = base.password;
            url.host = base.host;
            url.port = base.port;
            url.path = base.path.slice();
            url.query = base.query;
            if (c === '?') {
              url.query = '';
              state = 'query';
            } else if (c === '#') {
              url.fragment = '';
              state = 'fragment';
            } else if (c !== '') {
              url.query = null;
              shortenPath(url);
              state = 'path';
              p--;
            }
          }
          break;

        case 'relative slash':
          if (isSpecial(url) && (c === '/' || c === '\\')) {
            state = 'special authority ignore slashes';
          } else if (c === '/') {
            state = 'authority';
          } else {
            url.username = base.username;
            url.password = base.password;
            url.host = base.host;
            url.port = base.port;
            state = 'path';
            p--;
          }
          break;

        case 'special authority slashes':
          state = 'special authority ignore slashes';
          if (c === '/' && s[p + 1] === '/') p++;
          else p--;
          break;

        case 'special authority ignore slashes':
          if (c !== '/' && c !== '\\') {
            state = 'authority';
            p--;
          }
          break;

        case 'authority':
          if (c === '@') {
            if (atSignSeen) buffer = '%40' + buffer;
            atSignSeen = true;
            for (const cp of buffer) {
              if (cp === ':' && !passwordTokenSeen) {
                passwordTokenSeen = true;
                continue;
              }
              const encoded = percentEncode(cp, USERINFO_SET);
              if (passwordTokenSeen) url.password += encoded;
              else url.username += encoded;
            }
            buffer = '';
          } else if (c === '' || c === '/' || c === '?' || c === '#' || (isSpecial(url) && c === '\\')) {
            if (atSignSeen && buffer === '') return null;
            p -= Array.from(buffer).length + 1;
            buffer = '';
            state = 'host';
          } else {
            buffer += c;
          }
          break;

        case 'host':
        case 'hostname':
          if (override && url.scheme === 'file') {
            p--;
            state = 'file host';
          } else if (c === ':' && !insideBrackets) {
            if (buffer === '') return null;
            if (override === 'hostname') return null;
            const host = parseHost(buffer, !isSpecial(url));
            if (host === null) return null;
            url.host = host;
            buffer = '';
            state = 'port';
          } else if (c === '' || c === '/' || c === '?' || c === '#' || (isSpecial(url) && c === '\\')) {
            p--;
            if (isSpecial(url) && buffer === '') return null;
            if (override && buffer === '' && (includesCredentials(url) || url.port !== null)) return null;
            const host = parseHost(buffer, !isSpecial(url));
            if (host === null) return null;
            url.host = host;
            buffer = '';
            state = 'path start';
            if (override) return url;
          } else {
            if (c === '[') insideBrackets = true;
            if (c === ']') insideBrackets = false;
            buffer += c;
          }
          break;

        case 'port':
          if (/^[0-9]$/.test(c)) {
            buffer += c;
          } else if (c === '' || c === '/' || c === '?' || c === '#' || (isSpecial(url) && c === '\\') || override) {
            if (buffer !== '') {
              const port = parseInt(buffer, 10);
              if (port > 65535) return null;
              url.port = port === SPECIAL.get(url.scheme) ? null : port;
              buffer = '';
            }
            if (override) return url;
            state = 'path start';
            p--;
          } else {
            return null;
          }
          break;

        case 'file':
          url.scheme = 'file';
          url.host = '';
          if (c === '/' || c === '\\') {
            state = 'file slash';
          } else if (base && base.scheme === 'file') {
            url.host = base.host;
            url.path = base.path.slice();
            url.query = base.query;
            if (c === '?') {
              url.query = '';
              state = 'query';
            } else if (c === '#') {
              url.fragment = '';
              state = 'fragment';
            } else if (c !== '') {
              url.query = null;
              if (!startsWithWindowsDriveLetter(s, p)) shortenPath(url);
              else url.path = [];
              state = 'path';
              p--;
            }
          } else {
            state = 'path';
            p--;
          }
          break;

        case 'file slash':
          if (c === '/' || c === '\\') {
            state = 'file host';
          } else {
            if (base && base.scheme === 'file') {
              url.host = base.host;
              if (!startsWithWindowsDriveLetter(s, p) && isNormalizedWindowsDriveLetter(base.path[0] || '')) {
                url.path.push(base.path[0]);
              }
            }
            state = 'path';
            p--;
          }
          break;

        case 'file host':
          if (c === '' || c === '/' || c === '\\' || c === '?' || c === '#') {
            p--;
            if (!override && isWindowsDriveLetter(buffer)) {
              state = 'path'; // The buffer is the path's first segment.
            } else if (buffer === '') {
              url.host = '';
              if (override) return url;
              state = 'path start';
            } else {
              let host = parseHost(buffer, !isSpecial(url));
              if (host === null) return null;
              if (host === 'localhost') host = '';
              url.host = host;
              if (override) return url;
              buffer = '';
              state = 'path start';
            }
          } else {
            buffer += c;
          }
          break;

        case 'path start':
          if (isSpecial(url)) {
            state = 'path';
            if (c !== '/' && c !== '\\') p--;
          } else if (!override && c === '?') {
            url.query = '';
            state = 'query';
          } else if (!override && c === '#') {
            url.fragment = '';
            state = 'fragment';
          } else if (c !== '') {
            state = 'path';
            if (c !== '/') p--;
          } else if (override && url.host === null) {
            url.path.push('');
          }
          break;

        case 'path': {
          const slash = c === '/' || (isSpecial(url) && c === '\\');
          if (c === '' || slash || (!override && (c === '?' || c === '#'))) {
            if (isDoubleDot(buffer)) {
              shortenPath(url);
              if (!slash) url.path.push('');
            } else if (isSingleDot(buffer) && !slash) {
              url.path.push('');
            } else if (!isSingleDot(buffer)) {
              if (url.scheme === 'file' && url.path.length === 0 && isWindowsDriveLetter(buffer)) {
                buffer = buffer[0] + ':';
              }
              url.path.push(buffer);
            }
            buffer = '';
            if (c === '?') {
              url.query = '';
              state = 'query';
            }
            if (c === '#') {
              url.fragment = '';
              state = 'fragment';
            }
          } else {
            buffer += percentEncode(c, PATH_SET);
          }
          break;
        }

        case 'opaque path':
          if (c === '?') {
            url.query = '';
            state = 'query';
          } else if (c === '#') {
            url.fragment = '';
            state = 'fragment';
          } else if (c === ' ' && (s[p + 1] === '?' || s[p + 1] === '#')) {
            url.path += '%20';
          } else if (c !== '') {
            url.path += percentEncode(c, inC0);
          }
          break;

        case 'query':
          if ((!override && c === '#') || c === '') {
            url.query += percentEncode(buffer, isSpecial(url) ? SPECIAL_QUERY_SET : QUERY_SET);
            buffer = '';
            if (c === '#') {
              url.fragment = '';
              state = 'fragment';
            }
          } else {
            buffer += c;
          }
          break;

        case 'fragment':
          if (c !== '') url.fragment += percentEncode(c, FRAGMENT_SET);
          break;
      }
    }
    return url;
  }

  // "URL path serializer"
  const serializePath = (url) => (hasOpaquePath(url) ? url.path : url.path.map((seg) => '/' + seg).join(''));

  // "URL serializer" (4.5)
  function serializeURL(url, excludeFragment) {
    let out = url.scheme + ':';
    if (url.host !== null) {
      out += '//';
      if (includesCredentials(url)) {
        out += url.username + (url.password !== '' ? ':' + url.password : '') + '@';
      }
      out += url.host + (url.port !== null ? ':' + url.port : '');
    } else if (!hasOpaquePath(url) && url.path.length > 1 && url.path[0] === '') {
      out += '/.';
    }
    out += serializePath(url);
    if (url.query !== null) out += '?' + url.query;
    if (!excludeFragment && url.fragment !== null) out += '#' + url.fragment;
    return out;
  }

  // "origin" (4.7), serialized.
  function serializeOrigin(url) {
    switch (url.scheme) {
      case 'blob': {
        const inner = parseURL(serializePath(url), null);
        return inner && (inner.scheme === 'http' || inner.scheme === 'https') ? serializeOrigin(inner) : 'null';
      }
      case 'ftp': case 'http': case 'https': case 'ws': case 'wss':
        return url.scheme + '://' + url.host + (url.port !== null ? ':' + url.port : '');
      default:
        return 'null';
    }
  }

  // ---------------------------------------------------------------------
  // URL Standard: application/x-www-form-urlencoded (section 5)

  const decodeForm = (s) => utf8DecodeNoBOM(percentDecode(utf8Encode(s.replace(/\+/g, ' '))));

  function parseForm(input) {
    const list = [];
    for (const seq of input.split('&')) {
      if (seq === '') continue;
      const eq = seq.indexOf('=');
      list.push(eq < 0 ? [decodeForm(seq), ''] : [decodeForm(seq.slice(0, eq)), decodeForm(seq.slice(eq + 1))]);
    }
    return list;
  }

  function encodeForm(s) {
    let out = '';
    for (const b of utf8Encode(s)) {
      const ch = String.fromCharCode(b);
      out += b === 0x20 ? '+' : /[*\-._0-9A-Za-z]/.test(ch) ? ch : hex2(b);
    }
    return out;
  }

  const serializeForm = (list) => list.map(([name, value]) => encodeForm(name) + '=' + encodeForm(value)).join('&');

  // ---------------------------------------------------------------------
  // URL Standard: API (section 6)

  const urlState = new WeakMap(); // URL -> {url: record, params}
  const paramsState = new WeakMap(); // URLSearchParams -> {list, owner: URL | null}

  function internal(map, object) {
    const state = map.get(object);
    if (!state) throw new TypeError('Illegal invocation');
    return state;
  }

  function requireArgs(count, needed, name) {
    if (count < needed) throw new TypeError(`${name} requires ${needed} argument${needed > 1 ? 's' : ''}`);
  }

  class URL {
    constructor(url, base = undefined) {
      requireArgs(arguments.length, 1, "URL constructor");
      const record = parse(url, base);
      if (!record) throw new TypeError(`Invalid URL: ${url}`);
      const params = new URLSearchParams();
      const ps = paramsState.get(params);
      ps.owner = this;
      ps.list = record.query === null ? [] : parseForm(record.query);
      urlState.set(this, { url: record, params });
    }

    static canParse(url, base = undefined) {
      return parse(url, base) !== null;
    }

    static parse(url, base = undefined) {
      try {
        return new URL(url, base);
      } catch (e) {
        return null;
      }
    }

    get href() { return serializeURL(internal(urlState, this).url); }
    set href(value) {
      const state = internal(urlState, this);
      const record = parseURL(usv(value), null);
      if (!record) throw new TypeError(`Invalid URL: ${value}`);
      state.url = record;
      paramsState.get(state.params).list = record.query === null ? [] : parseForm(record.query);
    }

    get origin() { return serializeOrigin(internal(urlState, this).url); }

    get protocol() { return internal(urlState, this).url.scheme + ':'; }
    set protocol(value) {
      parseURL(usv(value) + ':', null, internal(urlState, this).url, 'scheme start');
    }

    get username() { return internal(urlState, this).url.username; }
    set username(value) {
      const url = internal(urlState, this).url;
      if (!cannotHaveCredentialsOrPort(url)) url.username = percentEncode(usv(value), USERINFO_SET);
    }

    get password() { return internal(urlState, this).url.password; }
    set password(value) {
      const url = internal(urlState, this).url;
      if (!cannotHaveCredentialsOrPort(url)) url.password = percentEncode(usv(value), USERINFO_SET);
    }

    get host() {
      const url = internal(urlState, this).url;
      if (url.host === null) return '';
      return url.port === null ? url.host : url.host + ':' + url.port;
    }
    set host(value) {
      const url = internal(urlState, this).url;
      if (!hasOpaquePath(url)) parseURL(usv(value), null, url, 'host');
    }

    get hostname() { return internal(urlState, this).url.host || ''; }
    set hostname(value) {
      const url = internal(urlState, this).url;
      if (!hasOpaquePath(url)) parseURL(usv(value), null, url, 'hostname');
    }

    get port() {
      const port = internal(urlState, this).url.port;
      return port === null ? '' : String(port);
    }
    set port(value) {
      const url = internal(urlState, this).url;
      if (cannotHaveCredentialsOrPort(url)) return;
      value = usv(value);
      if (value === '') url.port = null;
      else parseURL(value, null, url, 'port');
    }

    get pathname() { return serializePath(internal(urlState, this).url); }
    set pathname(value) {
      const url = internal(urlState, this).url;
      if (hasOpaquePath(url)) return;
      url.path = [];
      parseURL(usv(value), null, url, 'path start');
    }

    get search() {
      const query = internal(urlState, this).url.query;
      return query ? '?' + query : '';
    }
    set search(value) {
      const state = internal(urlState, this);
      const url = state.url;
      value = usv(value);
      const ps = paramsState.get(state.params);
      if (value === '') {
        url.query = null;
        ps.list = [];
        stripTrailingSpaces(url);
        return;
      }
      const input = value[0] === '?' ? value.slice(1) : value;
      url.query = '';
      parseURL(input, null, url, 'query');
      ps.list = parseForm(input);
    }

    get searchParams() { return internal(urlState, this).params; }

    get hash() {
      const fragment = internal(urlState, this).url.fragment;
      return fragment ? '#' + fragment : '';
    }
    set hash(value) {
      const url = internal(urlState, this).url;
      value = usv(value);
      if (value === '') {
        url.fragment = null;
        stripTrailingSpaces(url);
        return;
      }
      url.fragment = '';
      parseURL(value[0] === '#' ? value.slice(1) : value, null, url, 'fragment');
    }

    toString() { return this.href; }
    toJSON() { return this.href; }
  }

  // "API URL parser": the record, or null.
  function parse(url, base) {
    let baseRecord = null;
    if (base !== undefined) {
      baseRecord = parseURL(usv(base), null);
      if (!baseRecord) return null;
    }
    return parseURL(usv(url), baseRecord);
  }

  class URLSearchParams {
    constructor(init = '') {
      let list = [];
      if (init !== null && (typeof init === 'object' || typeof init === 'function')) {
        const iterator = init[Symbol.iterator];
        if (iterator !== undefined) {
          // sequence<sequence<USVString>>
          if (typeof iterator !== 'function') throw new TypeError('Query pairs must be iterable');
          for (const pair of init) {
            if (pair === null || (typeof pair !== 'object' && typeof pair !== 'function')) {
              throw new TypeError('Each query pair must be an iterable [name, value] tuple');
            }
            const items = Array.from(pair);
            if (items.length !== 2) throw new TypeError('Each query pair must be an iterable [name, value] tuple');
            list.push([usv(items[0]), usv(items[1])]);
          }
        } else {
          // record<USVString, USVString>
          for (const key of Reflect.ownKeys(init)) {
            const desc = Reflect.getOwnPropertyDescriptor(init, key);
            if (desc && desc.enumerable) list.push([usv(key), usv(init[key])]);
          }
        }
      } else {
        const s = usv(init);
        list = parseForm(s[0] === '?' ? s.slice(1) : s);
      }
      paramsState.set(this, { list, owner: null });
    }

    get size() { return internal(paramsState, this).list.length; }

    append(name, value) {
      requireArgs(arguments.length, 2, 'append');
      const state = internal(paramsState, this);
      state.list.push([usv(name), usv(value)]);
      update(state);
    }

    delete(name, value = undefined) {
      requireArgs(arguments.length, 1, 'delete');
      const state = internal(paramsState, this);
      name = usv(name);
      if (value !== undefined) value = usv(value);
      state.list = state.list.filter(([n, v]) => !(n === name && (value === undefined || v === value)));
      update(state);
    }

    get(name) {
      requireArgs(arguments.length, 1, 'get');
      name = usv(name);
      const pair = internal(paramsState, this).list.find(([n]) => n === name);
      return pair ? pair[1] : null;
    }

    getAll(name) {
      requireArgs(arguments.length, 1, 'getAll');
      name = usv(name);
      return internal(paramsState, this).list.filter(([n]) => n === name).map(([, v]) => v);
    }

    has(name, value = undefined) {
      requireArgs(arguments.length, 1, 'has');
      name = usv(name);
      if (value !== undefined) value = usv(value);
      return internal(paramsState, this).list.some(([n, v]) => n === name && (value === undefined || v === value));
    }

    set(name, value) {
      requireArgs(arguments.length, 2, 'set');
      const state = internal(paramsState, this);
      name = usv(name);
      value = usv(value);
      const i = state.list.findIndex(([n]) => n === name);
      if (i < 0) {
        state.list.push([name, value]);
      } else {
        state.list[i] = [name, value];
        state.list = state.list.filter(([n], j) => j <= i || n !== name);
      }
      update(state);
    }

    // Stable sort by name in UTF-16 code units (JS string order).
    sort() {
      const state = internal(paramsState, this);
      state.list.sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
      update(state);
    }

    forEach(callback, thisArg = undefined) {
      requireArgs(arguments.length, 1, 'forEach');
      if (typeof callback !== 'function') throw new TypeError('The callback must be a function');
      const state = internal(paramsState, this);
      for (let i = 0; i < state.list.length; i++) {
        callback.call(thisArg, state.list[i][1], state.list[i][0], this);
      }
    }

    keys() { return iterate(this, (pair) => pair[0]); }
    values() { return iterate(this, (pair) => pair[1]); }
    entries() { return iterate(this, (pair) => [pair[0], pair[1]]); }

    toString() { return serializeForm(internal(paramsState, this).list); }
  }
  URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;

  // Live iteration: reads the current list at each step, like the web.
  function* iterate(params, select) {
    const state = internal(paramsState, params);
    for (let i = 0; i < state.list.length; i++) yield select(state.list[i]);
  }

  // URLSearchParams "update steps": write the list back to the owning URL.
  function update(state) {
    if (!state.owner) return;
    const url = urlState.get(state.owner).url;
    const query = serializeForm(state.list);
    url.query = query === '' ? null : query;
    if (query === '') stripTrailingSpaces(url);
  }

  // ---------------------------------------------------------------------
  // Encoding Standard: API (section 8)

  const ENCODINGS = {
    'utf-8': ['unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'utf-8', 'utf8', 'x-unicode20utf8'],
    'utf-16le': ['csunicode', 'iso-10646-ucs-2', 'ucs-2', 'unicode', 'unicodefeff', 'utf-16', 'utf-16le'],
    'utf-16be': ['unicodefffe', 'utf-16be'],
    'windows-1252': ['ansi_x3.4-1968', 'ascii', 'cp1252', 'cp819', 'csisolatin1', 'ibm819', 'iso-8859-1',
      'iso-ir-100', 'iso8859-1', 'iso88591', 'iso_8859-1', 'iso_8859-1:1987', 'l1', 'latin1', 'us-ascii',
      'windows-1252', 'x-cp1252'],
  };
  const LABELS = new Map();
  for (const name of Object.keys(ENCODINGS)) for (const label of ENCODINGS[name]) LABELS.set(label, name);
  const DECODERS = {
    'utf-8': utf8Decoder,
    'utf-16le': () => utf16Decoder(false),
    'utf-16be': () => utf16Decoder(true),
    'windows-1252': windows1252Decoder,
  };

  const encoderState = new WeakSet();

  class TextEncoder {
    constructor() { encoderState.add(this); }

    get encoding() { return 'utf-8'; }

    encode(input = '') {
      return utf8Encode(usv(input));
    }

    // Encodes as much of `source` as fits in `destination` without
    // splitting a code point.
    encodeInto(source, destination) {
      requireArgs(arguments.length, 2, 'encodeInto');
      if (Object.prototype.toString.call(destination) !== '[object Uint8Array]') {
        throw new TypeError('The destination must be a Uint8Array');
      }
      source = usv(source);
      let read = 0, written = 0;
      for (const ch of source) {
        const bytes = utf8Encode(ch);
        if (written + bytes.length > destination.length) break;
        destination.set(bytes, written);
        written += bytes.length;
        read += ch.length;
      }
      return { read, written };
    }
  }

  const decoderState = new WeakMap();

  function toBytes(input) {
    if (input === undefined) return new Uint8Array(0);
    if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    const tag = Object.prototype.toString.call(input);
    if (tag === '[object ArrayBuffer]' || tag === '[object SharedArrayBuffer]') return new Uint8Array(input);
    throw new TypeError('The input must be an ArrayBuffer or an ArrayBufferView');
  }

  const fatalError = () => { throw new TypeError('The encoded data was not valid.'); };

  class TextDecoder {
    constructor(label = 'utf-8', options = undefined) {
      const name = LABELS.get(usv(label).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, '').toLowerCase());
      if (!name) throw new RangeError(`The encoding label provided ('${label}') is invalid.`);
      const opts = options == null ? {} : options;
      decoderState.set(this, {
        name,
        fatal: Boolean(opts.fatal),
        ignoreBOM: Boolean(opts.ignoreBOM),
        decoder: null,
        bomSeen: false,
      });
    }

    get encoding() { return internal(decoderState, this).name; }
    get fatal() { return internal(decoderState, this).fatal; }
    get ignoreBOM() { return internal(decoderState, this).ignoreBOM; }

    decode(input = undefined, options = undefined) {
      const state = internal(decoderState, this);
      const bytes = toBytes(input);
      const stream = Boolean(options != null && options.stream);
      if (!state.decoder) {
        state.decoder = DECODERS[state.name]();
        state.bomSeen = false;
      }
      const units = [];
      try {
        state.decoder(bytes, !stream, units, state.fatal ? fatalError : replace);
      } catch (e) {
        state.decoder = null;
        throw e;
      }
      if (!stream) state.decoder = null;
      // "serialize I/O queue": drop a leading BOM for the Unicode encodings.
      let start = 0;
      if (state.name !== 'windows-1252' && !state.ignoreBOM && !state.bomSeen && units.length) {
        state.bomSeen = true;
        if (units[0] === 0xfeff) start = 1;
      }
      return unitsToString(units, start);
    }
  }

  for (const [cls, tag] of [[URL, 'URL'], [URLSearchParams, 'URLSearchParams'],
    [TextEncoder, 'TextEncoder'], [TextDecoder, 'TextDecoder']]) {
    Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: tag, configurable: true });
    if (typeof global[tag] === 'undefined') {
      Object.defineProperty(global, tag, { value: cls, writable: true, configurable: true, enumerable: false });
    }
  }
})(globalThis);
