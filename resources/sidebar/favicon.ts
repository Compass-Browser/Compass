// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A board's tab (docs/design/easels.md, "In the sidebar"): an easel, in the
// sidebar's text color for light and dark.
const EASEL_URL = /^(compass|chrome):\/\/easel\/[0-9a-f-]{36}([?#/]|$)/i;
const EASEL_ICON = 'data:image/svg+xml,' + encodeURIComponent(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">' +
    '<style>*{fill:none;stroke:#3c4043;stroke-width:1.4;' +
    'stroke-linecap:round;stroke-linejoin:round}' +
    '@media (prefers-color-scheme:dark){*{stroke:#e8eaed}}</style>' +
    '<rect x="2.5" y="2.5" width="11" height="7.5" rx="1"/>' +
    '<path d="M5.5 10 3.8 14M10.5 10l1.7 4M8 10v3.5M8 1v1.5"/></svg>');

export function isEaselUrl(url: string): boolean {
  return EASEL_URL.test(url);
}

// The icon of |pageUrl| in profile |profileId| (a ProfileOption ID), or in
// the page's own profile without one. The page can show another profile's
// entries: a preview of its space, or the window's own after a switch to its
// space, which keeps the page (docs/design/profiles-in-place.md).
export function getFaviconUrl(
    pageUrl: string, size: number, profileId: string|null): string {
  if (isEaselUrl(pageUrl)) {
    return EASEL_ICON;
  }
  const params = new URLSearchParams({
    size: `${size}`,
    scaleFactor: `${window.devicePixelRatio}x`,
    pageUrl,
    allowGoogleServerFallback: '0',
  });
  if (profileId) {
    params.set('profile', profileId);
  }
  return `chrome://compass-favicon/?${params.toString()}`;
}
