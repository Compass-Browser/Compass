// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AVFoundation
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
  /// Watches the page's videos:
  /// - the one playing with sound gets `autoPictureInPicture` (WebKit's
  ///   AVKit "start automatically from inline"), in case WebKit honors it
  ///   for apps: an app on its way out can't start one itself, and on iOS
  ///   27 it doesn't (only Safari's own standby does);
  /// - Compass is told whether there's one (`state:`), so a tab switch can
  ///   keep the page in the window before it starts it;
  /// - back inline from the window, still playing, is the window's "back to
  ///   the page".
  static var script: WKUserScript {
    WKUserScript(
      source: """
        (() => {
          const enabled = \(isOn);
          const post = (m) => window.webkit.messageHandlers.\(handler).postMessage(m);
          const eligible = (v) => !v.paused && !v.ended && !v.muted && v.volume > 0 && !v.disablePictureInPicture
            && v.offsetWidth >= 100 && v.offsetHeight >= 100;
          let last = "idle";
          const update = () => {
            const videos = [...document.querySelectorAll("video")];
            const playing = videos.filter(eligible);
            const one = enabled && playing.length === 1 ? playing[0] : null;
            for (const v of videos) if ("autoPictureInPicture" in v && v.webkitPresentationMode !== "picture-in-picture") {
              v.autoPictureInPicture = v === one;
            }
            if (one && !document.pictureInPictureElement) window.__compassAutoPiP = one;
            const state = one ? "playing" : "idle";
            if (state !== last) { last = state; post("state:" + state); }
          };
          for (const type of ["play", "playing", "pause", "ended", "volumechange", "emptied", "resize", "loadedmetadata"]) {
            document.addEventListener(type, update, true);
          }
          document.addEventListener("webkitpresentationmodechanged", (event) => {
            const video = event.target;
            const mode = video.webkitPresentationMode;
            if (video === window.__compassAutoPiP) post("log:mode " + mode);
            if (mode === "picture-in-picture" && video === window.__compassAutoPiP) video.__compassInWindow = true;
            // Back inline after being in the window, still playing: the
            // window's "back to the page" (not a start that failed).
            if (mode !== "inline" || !video.__compassInWindow) return;
            video.__compassInWindow = false;
            setTimeout(() => { if (!video.paused) post("restore"); }, 400);
          }, true);
        })();
        """,
      injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: world)
  }

  /// "entered" when it moved a video to picture-in-picture, else why not.
  static let enterSource = """
    (() => {
      const videos = [...document.querySelectorAll("video")];
      if (!videos.length) return "no video";
      if (document.pictureInPictureElement) return "already in PiP";
      if (videos.some((v) => v.webkitPresentationMode && v.webkitPresentationMode !== "inline")) return "already presented";
      const why = (v) => v.paused ? "paused" : v.ended ? "ended" : v.muted || v.volume === 0 ? "muted"
        : v.readyState < 2 ? "not ready" : v.disablePictureInPicture ? "PiP disabled by page"
        : v.offsetWidth < 100 || v.offsetHeight < 100 ? `small ${v.offsetWidth}x${v.offsetHeight}` : "";
      const candidates = videos.filter((v) => !why(v));
      if (candidates.length !== 1) return `${candidates.length} candidates (${videos.map(why).join(", ")})`;
      const video = candidates[0];
      window.__compassAutoPiP = video;
      if (video.requestPictureInPicture) {
        video.requestPictureInPicture().catch((e) => {
          window.webkit.messageHandlers.\(handler).postMessage("log:request failed " + e.name + ": " + e.message);
          if (video.webkitSupportsPresentationMode && video.webkitSupportsPresentationMode("picture-in-picture")) {
            video.webkitSetPresentationMode("picture-in-picture");
          } else {
            window.__compassAutoPiP = null;
          }
        });
      } else if (video.webkitSupportsPresentationMode && video.webkitSupportsPresentationMode("picture-in-picture")) {
        video.webkitSetPresentationMode("picture-in-picture");
      } else {
        window.__compassAutoPiP = null;
        return "PiP not supported";
      }
      return "entered";
    })()
    """

  static let exitSource = """
    (() => {
      const video = window.__compassAutoPiP;
      if (!video) return false;
      window.__compassAutoPiP = null;
      video.__compassInWindow = false;
      if (document.pictureInPictureElement === video) document.exitPictureInPicture().catch(() => {});
      else if (video.webkitPresentationMode === "picture-in-picture") video.webkitSetPresentationMode("inline");
      return true;
    })()
    """

  static func enter(_ view: WKWebView?) {
    guard isOn, let view else { return }
    view.evaluateJavaScript(enterSource, in: nil, in: world) { result in
      if case .success(let outcome) = result, outcome as? String != "entered" { unpark(view) }
      #if DEBUG
        switch result {
        case .success(let outcome): print("AutoPiP: enter: \(outcome)")
        case .failure(let error): print("AutoPiP: enter failed: \(error)")
        }
      #endif
    }
  }

  /// Picture-in-picture needs the playback category (WebKit leaves the
  /// app's session as it is): set once at launch; it's only activated when
  /// a page plays sound. Media also plays with the silent switch on, as in
  /// Safari.
  static func configureAudioSession() {
    do {
      try AVAudioSession.sharedInstance().setCategory(.playback, mode: .moviePlayback)
    } catch {
      #if DEBUG
        print("AutoPiP: audio session: \(error)")
      #endif
    }
  }

  /// A web view out of the window ends its picture-in-picture: the tab
  /// left, when its page plays a video, moves here before SwiftUI drops
  /// it, in the window but not visible, until it's shown again or the
  /// window closes.
  private static var parking: UIView?

  static func park(_ view: WKWebView) {
    guard parking == nil || view.superview !== parking, let window = UIApplication.shared.connectedScenes
      .compactMap({ ($0 as? UIWindowScene)?.keyWindow }).first
    else { return }
    let lot = parking ?? {
      let lot = UIView(frame: CGRect(x: 0, y: 0, width: 1, height: 1))
      lot.clipsToBounds = true
      lot.isUserInteractionEnabled = false
      lot.alpha = 0.01
      parking = lot
      return lot
    }()
    if lot.superview !== window { window.insertSubview(lot, at: 0) }
    let size = view.bounds.size
    lot.addSubview(view)
    view.frame = CGRect(origin: .zero, size: size)
    #if DEBUG
      print("AutoPiP: parked the tab's view")
    #endif
  }

  /// Its window closed (or the tab came back, SwiftUI moved it): out of
  /// the lot.
  static func unpark(_ view: WKWebView) {
    if let parking, view.superview === parking { view.removeFromSuperview() }
  }

  static func exit(_ view: WKWebView?) {
    guard let view else { return }
    view.evaluateJavaScript(exitSource, in: nil, in: world, completionHandler: nil)
  }
}
