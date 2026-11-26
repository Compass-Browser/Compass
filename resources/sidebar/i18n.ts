// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's localized strings (strings/compass_strings.grd), which every
// page served from this pak gets in loadTimeData
// (browser/ui/webui/localized_strings.h). Markup in a settings section's
// .html file uses $i18n{key} instead.

import '/strings.m.js';

import {loadTimeData} from '//resources/js/load_time_data.js';

/** The string |key|. */
export function i18n(key: string): string {
  return loadTimeData.getString(key);
}

/** The string |key|, with $1, $2, ... replaced by |args|. */
export function i18nF(key: string, ...args: Array<string|number>): string {
  return loadTimeData.getStringF(key, ...args);
}

/**
 * The plural message |key|, in ICU syntax
 * ("{COUNT, plural, =1 {1 tab} other {# tabs}}"), for |count|: # becomes the
 * count, and $1, $2, ... |args|. As ICU does natively, an exact =N branch
 * wins, then the branch of the language's plural category, then "other".
 * A message that isn't a plural is returned as it is, with |args|.
 */
export function i18nPlural(
    key: string, count: number, ...args: Array<string|number>): string {
  const message = loadTimeData.getString(key);
  const branches = parsePlural(message);
  if (!branches) {
    return loadTimeData.substituteString(message, ...args);
  }
  const language = loadTimeData.valueExists('language') ?
      loadTimeData.getString('language') :
      undefined;
  const text = branches.get(`=${count}`) ??
      branches.get(new Intl.PluralRules(language).select(count)) ??
      branches.get('other') ?? '';
  const formatted = text.replace(
      /#/g, () => new Intl.NumberFormat(language).format(count));
  return loadTimeData.substituteString(formatted, ...args);
}

/**
 * The branches of a message made of one plural argument, by selector
 * ("=1", "one", "other"), or null if it isn't one.
 */
function parsePlural(message: string): Map<string, string>|null {
  const head = /^\s*\{\s*\w+\s*,\s*plural\s*,/.exec(message);
  if (!head) {
    return null;
  }
  const branches = new Map<string, string>();
  let i = head[0].length;
  while (i < message.length) {
    const selector = /^\s*(=\d+|\w+)\s*\{/.exec(message.slice(i));
    if (!selector) {
      break;
    }
    i += selector[0].length;
    let depth = 1;
    const start = i;
    while (i < message.length && depth > 0) {
      if (message[i] === '{') {
        depth++;
      } else if (message[i] === '}') {
        depth--;
      }
      i++;
    }
    if (depth !== 0) {
      return null;
    }
    branches.set(selector[1]!, message.slice(start, i - 1));
  }
  return branches.has('other') ? branches : null;
}
