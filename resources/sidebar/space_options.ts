// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What the sidebar's space menu and the settings window's Spaces pane both
// offer for a space.

export interface ArchiveDelay {
  hours: number;
  label: string;
}

// The auto-archive delays offered, in hours; 0 is never.
export const ARCHIVE_DELAYS: ArchiveDelay[] = [
  {hours: 12, label: '12 hours'},
  {hours: 24, label: '24 hours'},
  {hours: 7 * 24, label: '7 days'},
  {hours: 30 * 24, label: '30 days'},
  {hours: 0, label: 'Never'},
];

// The first character of |text|: a whole emoji sequence, for an emoji icon.
export function firstGrapheme(text: string): string {
  const segments = new Intl.Segmenter(undefined, {granularity: 'grapheme'});
  for (const {segment} of segments.segment(text)) {
    return segment;
  }
  return '';
}
