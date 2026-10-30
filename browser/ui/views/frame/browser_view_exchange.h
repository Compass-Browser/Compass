// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_EXCHANGE_H_
#define STRATA_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_EXCHANGE_H_

class BrowserView;
class BrowserWidget;

namespace strata {

// Puts two browser windows' contents in each other's native window: |a|'s
// BrowserView (with its Browser, profile and tabs) moves into |b|'s native
// window and |b|'s into |a|'s. A window of another profile thereby takes the
// place of a shown one without a second native window showing. See
// docs/design/profiles-in-place.md. Upstream's window classes friend this.
class BrowserViewExchange {
 public:
  // |a| is the shown one: its bubbles close.
  static void Exchange(BrowserView& a, BrowserView& b);

 private:
  // Drops what |view| registered with its widget.
  static void Unbind(BrowserView& view);
  // Redoes, for the widget |view| now lives in, what AddedToWidget() did
  // once for the first.
  static void Rebind(BrowserView& view);
  // Points |widget|'s native widget at |view| (platform specific).
  static void SetNativeWidgetView(BrowserWidget& widget, BrowserView* view);
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_EXCHANGE_H_
