// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {i18n, i18nPlural} from './i18n.js';

// What the sidebar's space menu and the settings window's Spaces pane both
// offer for a space.

export interface ArchiveDelay {
  hours: number;
  label: string;
}

// The auto-archive delays offered, in hours; 0 is never.
export const ARCHIVE_DELAYS: ArchiveDelay[] = [
  {hours: 12, label: i18nPlural('spaceOptionsHours', 12)},
  {hours: 24, label: i18nPlural('spaceOptionsHours', 24)},
  {hours: 7 * 24, label: i18nPlural('spaceOptionsDays', 7)},
  {hours: 30 * 24, label: i18nPlural('spaceOptionsDays', 30)},
  {hours: 0, label: i18n('spaceOptionsNever')},
];

// The first character of |text|: a whole emoji sequence, for an emoji icon.
export function firstGrapheme(text: string): string {
  const segments = new Intl.Segmenter(undefined, {granularity: 'grapheme'});
  for (const {segment} of segments.segment(text)) {
    return segment;
  }
  return '';
}
