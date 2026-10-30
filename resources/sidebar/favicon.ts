// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The icon of |pageUrl| in profile |profileId| (a ProfileOption ID), or in
// the page's own profile without one. The page can show another profile's
// entries: a preview of its space, or the window's own after a switch to its
// space, which keeps the page (docs/design/profiles-in-place.md).
export function getFaviconUrl(
    pageUrl: string, size: number, profileId: string|null): string {
  const params = new URLSearchParams({
    size: `${size}`,
    scaleFactor: `${window.devicePixelRatio}x`,
    pageUrl,
    allowGoogleServerFallback: '0',
  });
  if (profileId) {
    params.set('profile', profileId);
  }
  return `chrome://strata-favicon/?${params.toString()}`;
}
