// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import WebKit

/// Auto picture-in-picture, as on the desktop (docs/design/picture-in-
/// picture.md): leaving a tab whose video plays with sound shows the video
/// in picture-in-picture, and coming back to the tab puts it back in the
/// page. On the phone, leaving the app counts too.
///
/// The desktop's conditions, checked in the page: the video plays, isn't
/// muted, is at least 100×100, and is the only such one in the main frame;
/// a video in picture-in-picture already (the user's) is left alone.
/// Scripts the app evaluates count as a user gesture, so WebKit lets the
/// page enter picture-in-picture. The window's "back to the page" button
/// puts the video back inline still playing; the page reports that, and
/// Compass shows the tab (its close button pauses the video: nothing).
@MainActor
enum AutoPiP {
  static let enabledKey = "AutoPictureInPicture"
  static var isOn: Bool { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }

  /// Its own world: the page doesn't see which video Compass moved.
  static let world = WKContentWorld.world(name: "CompassPiP")
  static let handler = "compassPiP"

  /// Watches the video Compass moved: back inline and still playing is the
  /// window's "back to the page".
  static var script: WKUserScript {
    WKUserScript(
      source: """
        document.addEventListener("webkitpresentationmodechanged", (event) => {
          const video = event.target;
          if (video !== window.__compassAutoPiP || video.webkitPresentationMode !== "inline") return;
          window.__compassAutoPiP = null;
          setTimeout(() => { if (!video.paused) window.webkit.messageHandlers.\(handler).postMessage("restore"); }, 400);
        }, true);
        """,
      injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: world)
  }

  /// true when it moved a video to picture-in-picture.
  static let enterSource = """
    (() => {
      const playing = (v) => !v.paused && !v.ended && !v.muted && v.volume > 0 && v.readyState >= 2
        && !v.disablePictureInPicture && v.offsetWidth >= 100 && v.offsetHeight >= 100;
      if (document.pictureInPictureElement) return false;
      const videos = [...document.querySelectorAll("video")];
      if (videos.some((v) => v.webkitPresentationMode && v.webkitPresentationMode !== "inline")) return false;
      const candidates = videos.filter(playing);
      if (candidates.length !== 1) return false;
      const video = candidates[0];
      window.__compassAutoPiP = video;
      if (video.webkitSupportsPresentationMode && video.webkitSupportsPresentationMode("picture-in-picture")) {
        video.webkitSetPresentationMode("picture-in-picture");
      } else if (video.requestPictureInPicture) {
        video.requestPictureInPicture().catch(() => { window.__compassAutoPiP = null; });
      } else {
        window.__compassAutoPiP = null;
        return false;
      }
      return true;
    })()
    """

  static let exitSource = """
    (() => {
      const video = window.__compassAutoPiP;
      if (!video) return false;
      window.__compassAutoPiP = null;
      if (video.webkitPresentationMode === "picture-in-picture") video.webkitSetPresentationMode("inline");
      else if (document.pictureInPictureElement === video) document.exitPictureInPicture().catch(() => {});
      return true;
    })()
    """

  static func enter(_ view: WKWebView?) {
    guard isOn, let view else { return }
    view.evaluateJavaScript(enterSource, in: nil, in: world) { result in
      #if DEBUG
        if case .success(let moved) = result, moved as? Bool == true { print("AutoPiP: entered") }
        if case .failure(let error) = result { print("AutoPiP: enter failed: \(error)") }
      #endif
    }
  }

  static func exit(_ view: WKWebView?) {
    guard let view else { return }
    view.evaluateJavaScript(exitSource, in: nil, in: world, completionHandler: nil)
  }
}
