// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import WebKit

/// The desktop site's viewport. On the iPhone, WebKit's desktop content mode
/// changes the user agent but still honors a page's `width=device-width`,
/// so media queries see a phone. This sets the page's viewport to 980 px
/// (Safari's desktop width) whenever the desktop user agent is in use, that
/// is when the site's Desktop Site setting is on.
@MainActor
enum DesktopViewport {
  static let width = 980

  static var script: WKUserScript { WKUserScript(
    source: """
      (() => {
        if (!/Macintosh/.test(navigator.userAgent)) return;
        const content = 'width=\(width)';
        const apply = () => {
          const metas = document.querySelectorAll('meta[name="viewport"]');
          if (metas.length === 0) {
            const meta = document.createElement('meta');
            meta.name = 'viewport';
            meta.content = content;
            (document.head || document.documentElement)?.appendChild(meta);
          }
          metas.forEach(m => { if (m.content !== content) m.content = content; });
        };
        apply();
        // Pages add or change theirs while loading.
        const observer = new MutationObserver(apply);
        if (document.documentElement) {
          observer.observe(document.documentElement, {
            childList: true, subtree: true, attributes: true, attributeFilter: ['content'] });
        }
        document.addEventListener('DOMContentLoaded', () => { apply(); observer.disconnect(); });
      })();
      """,
    injectionTime: .atDocumentStart, forMainFrameOnly: true) }
}
