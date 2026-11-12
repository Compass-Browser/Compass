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
/// A call (Teams, Zoom: common/picture_in_picture/call_pip.js, as on the
/// desktop) counts as playing while it's on, and shows its largest remote
/// tile: call tiles are muted, and there are several.
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

  /// What auto picture-in-picture did lately (the page's reports, each tab
  /// switch's decision, each attempt's outcome), newest last: Settings →
  /// Auto Picture in Picture → Copy Diagnostics, to see on a device why a
  /// window didn't open.
  @MainActor private(set) static var recent: [String] = []

  @MainActor static func note(_ line: String) {
    let time = Date().formatted(.dateTime.hour(.twoDigits(amPM: .omitted)).minute().second())
    recent.append("\(time) \(line)")
    if recent.count > 60 { recent.removeFirst(recent.count - 60) }
    #if DEBUG
      print("AutoPiP: \(line)")
    #endif
  }
  static let handler = "compassPiP"

  /// The desktop's call script, bundled (common/picture_in_picture/): in
  /// this world, it leaves pages other than calls alone. On iOS it picks
  /// the tile; WebKit wants a gesture for each picture-in-picture request,
  /// so the window doesn't follow the speaker as on the desktop.
  static var callScript: WKUserScript? {
    guard let url = Bundle.main.url(forResource: "call_pip", withExtension: "js"),
      let source = try? String(contentsOf: url, encoding: .utf8)
    else { return nil }
    return WKUserScript(source: source, injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: world)
  }

  /// Watches the page's videos:
  /// - the one playing with sound gets `autoPictureInPicture` (WebKit's
  ///   AVKit "start automatically from inline"), in case WebKit honors it
  ///   for apps: an app on its way out can't start one itself, and on iOS
  ///   27 it doesn't (only Safari's own standby does);
  /// - Compass is told whether there's one (`state:`), so a tab switch can
  ///   keep the page in the window before it starts it;
  /// - back inline from the window, still playing, is the window's "back to
  ///   the page";
  /// - a playing video scrolled off screen (under half visible) is reported
  ///   (`scroll:out`), and the one Compass moved for it again once 90% of
  ///   it is back (`scroll:in`): only a script the app evaluates may start
  ///   picture-in-picture, so the app answers with enterSource/exitSource.
  ///   After its window closes or goes back to the page, a video isn't
  ///   moved again until it has been on screen.
  /// Videos play in the page, not fullscreen: on the iPhone WebKit plays a
  /// video inline only if it has `playsinline`, else it goes fullscreen as
  /// it starts (YouTube's player, for one). Every video gets it as it's
  /// added, before it can play; the player's own fullscreen button still
  /// works. Main frame and frames alike (embeds), from the document's start.
  static let inlineScript = WKUserScript(
    source: """
      (() => {
        const inline = (video) => {
          if (video.playsInline) return;
          video.playsInline = true;
          video.setAttribute("playsinline", "");
          video.setAttribute("webkit-playsinline", "");
        };
        const scan = (root) => {
          if (root instanceof HTMLVideoElement) inline(root);
          if (root.querySelectorAll) for (const video of root.querySelectorAll("video")) inline(video);
        };
        new MutationObserver((records) => {
          for (const record of records) for (const node of record.addedNodes) scan(node);
        }).observe(document, { childList: true, subtree: true });
        scan(document);
        // A video made with document.createElement and played before it's
        // in the page.
        document.addEventListener("play", (event) => {
          if (event.target instanceof HTMLVideoElement) inline(event.target);
        }, true);
      })();
      """,
    injectionTime: .atDocumentStart, forMainFrameOnly: false, in: world)

  static var script: WKUserScript {
    WKUserScript(
      source: """
        (() => {
          const enabled = \(isOn);
          const post = (m) => window.webkit.messageHandlers.\(handler).postMessage(m);
          // disablePictureInPicture doesn't count: YouTube sets it on its
          // player, and the window is what Compass offers (the enter script
          // clears it), as for calls.
          const eligible = (v) => !v.paused && !v.ended && !v.muted && v.volume > 0
            && v.offsetWidth >= 100 && v.offsetHeight >= 100;
          let last = "idle";
          const update = () => {
            const videos = [...document.querySelectorAll("video")];
            const playing = videos.filter(eligible);
            const one = enabled && playing.length === 1 ? playing[0] : null;
            const call = enabled && !!globalThis.compassCallPip?.inCall();
            for (const v of videos) if ("autoPictureInPicture" in v && v.webkitPresentationMode !== "picture-in-picture") {
              v.autoPictureInPicture = v === one;
            }
            // Not document.pictureInPictureElement: on YouTube WebKit keeps
            // reporting one while no window is open. A video's own mode is
            // what's on screen.
            const inPiP = videos.some((v) => v.webkitPresentationMode === "picture-in-picture");
            if (one && !inPiP) window.__compassAutoPiP = one;
            const state = one || call ? "playing" : "idle";
            if (state !== last) { last = state; post("state:" + state); }
          };
          for (const type of ["play", "playing", "pause", "ended", "volumechange", "emptied", "resize", "loadedmetadata"]) {
            document.addEventListener(type, update, true);
          }
          // A call starts and ends with no media event of its own.
          if (globalThis.compassCallPip) setInterval(update, 1000);
          document.addEventListener("webkitpresentationmodechanged", (event) => {
            const video = event.target;
            const mode = video.webkitPresentationMode;
            if (video === window.__compassAutoPiP) post("log:mode " + mode);
            if (mode === "picture-in-picture" && video === window.__compassAutoPiP) video.__compassInWindow = true;
            // Back inline after being in the window, still playing: the
            // window's "back to the page" (not a start that failed).
            if (mode !== "inline" || !video.__compassInWindow) return;
            video.__compassInWindow = false;
            if (window.__compassAutoPiPScroll === video) {
              // Its tab is the one shown: back to the page brings the video
              // into view instead.
              window.__compassAutoPiPScroll = null;
              if ((ratios.get(video) ?? 0) >= 0.9) return;
              held.add(video);
              setTimeout(() => { if (!video.paused) video.scrollIntoView({ block: "center" }); }, 400);
              return;
            }
            setTimeout(() => { if (!video.paused) post("restore"); }, 400);
          }, true);

          const ratios = new WeakMap();
          const held = new WeakSet();
          const observer = new IntersectionObserver((entries) => {
            for (const entry of entries) {
              const video = entry.target;
              // Visible share of what could be visible: a video taller than
              // the viewport counts as fully on screen when it fills it.
              const fits = Math.min(entry.boundingClientRect.height, entry.rootBounds?.height ?? Infinity);
              const ratio = entry.isIntersecting && fits > 0 ? Math.min(1, entry.intersectionRect.height / fits) : 0;
              ratios.set(video, ratio);
              // Out while half of it is still on screen, so the window grows
              // from the video and not from a sliver at the edge; back only
              // once (nearly) all of it is, so it lands where it's seen. The
              // gap keeps it from flipping in between.
              if (ratio >= 0.9) held.delete(video);
            }
            const want = window.__compassAutoPiPWant();
            if (want) post("scroll:" + want);
          }, { threshold: Array.from({ length: 21 }, (_, i) => i / 20) });
          // What scrolling asks for now: "out", "in" or "". The app asks
          // again once the page stands still, as a window started mid-scroll
          // grows from (or lands in) a frame that has moved on.
          window.__compassAutoPiPWant = () => {
            const moved = window.__compassAutoPiPScroll;
            if (moved) return (ratios.get(moved) ?? 0) >= 0.9 ? "in" : "";
            if (!enabled) return "";
            for (const v of document.querySelectorAll("video")) {
              if (ratios.has(v) && ratios.get(v) < 0.5 && eligible(v) && !held.has(v)) return "out";
            }
            return "";
          };
          // While Compass's window shows the video, the page doesn't hear it
          // went hidden: YouTube pauses on that (background play is a paid
          // feature there), which would end the window at once. A capture
          // listener on the window comes before the page's own on the
          // document.
          const inWindow = () => {
            const video = window.__compassAutoPiP;
            return !!video && (video.webkitPresentationMode === "picture-in-picture" || video.__compassEntering);
          };
          for (const type of ["visibilitychange", "webkitvisibilitychange", "pagehide", "blur"]) {
            window.addEventListener(type, (event) => {
              // The page's own (not a field's blur, which still has to work).
              if ((event.target === window || event.target === document) && inWindow()) {
                event.stopImmediatePropagation();
              }
            }, true);
          }

          // Watched from its first play; observing again reports where it
          // is now (a video started while off screen counts).
          const watched = new WeakSet();
          document.addEventListener("play", (event) => {
            const video = event.target;
            if (!(video instanceof HTMLVideoElement)) return;
            if (watched.has(video)) observer.unobserve(video);
            watched.add(video);
            observer.observe(video);
          }, true);
        })();
        """,
      injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: world)
  }

  /// "entered" when it moved a video to picture-in-picture, else why not.
  /// `scroll`: moved for leaving the screen (it comes back on scroll:in).
  static func enterSource(scroll: Bool) -> String {
    """
    ((scroll) => {
      const videos = [...document.querySelectorAll("video")];
      if (!videos.length) return "no video";
      // A video's own mode, not document.pictureInPictureElement, which
      // WebKit keeps set on YouTube with no window open.
      if (videos.some((v) => v.webkitPresentationMode === "picture-in-picture")) return "already in PiP";
      if (videos.some((v) => v.webkitPresentationMode && v.webkitPresentationMode !== "inline")) return "already presented";
      const why = (v) => v.paused ? "paused" : v.ended ? "ended" : v.muted || v.volume === 0 ? "muted"
        : v.readyState < 2 ? "not ready"
        : v.offsetWidth < 100 || v.offsetHeight < 100 ? `small ${v.offsetWidth}x${v.offsetHeight}` : "";
      // A call: its pick (the largest remote tile), muted as tiles are.
      const call = globalThis.compassCallPip?.inCall() ? globalThis.compassCallPip.pick() : null;
      const candidates = call ? [call] : videos.filter((v) => !why(v));
      if (candidates.length !== 1) return `${candidates.length} candidates (${videos.map(why).join(", ")})`;
      const video = candidates[0];
      // Sites switch it off (YouTube's player): the window is Compass's.
      if (video.disablePictureInPicture) video.disablePictureInPicture = false;
      // Until the window is up, the page's "hidden" is held back too.
      video.__compassEntering = true;
      setTimeout(() => { video.__compassEntering = false; }, 3000);
      window.__compassAutoPiP = video;
      if (scroll) window.__compassAutoPiPScroll = video;
      if (video.requestPictureInPicture) {
        video.requestPictureInPicture().catch((e) => {
          window.webkit.messageHandlers.\(handler).postMessage("log:request failed " + e.name + ": " + e.message);
          if (video.webkitSupportsPresentationMode && video.webkitSupportsPresentationMode("picture-in-picture")) {
            video.webkitSetPresentationMode("picture-in-picture");
          } else {
            window.__compassAutoPiP = null;
            window.__compassAutoPiPScroll = null;
          }
        });
      } else if (video.webkitSupportsPresentationMode && video.webkitSupportsPresentationMode("picture-in-picture")) {
        video.webkitSetPresentationMode("picture-in-picture");
      } else {
        window.__compassAutoPiP = null;
        window.__compassAutoPiPScroll = null;
        return "PiP not supported";
      }
      return "entered";
    })(\(scroll))
    """
  }

  static let exitSource = """
    (() => {
      const video = window.__compassAutoPiP;
      if (!video) return false;
      window.__compassAutoPiP = null;
      window.__compassAutoPiPScroll = null;
      video.__compassInWindow = false;
      if (document.pictureInPictureElement === video) document.exitPictureInPicture().catch(() => {});
      else if (video.webkitPresentationMode === "picture-in-picture") video.webkitSetPresentationMode("inline");
      return true;
    })()
    """

  /// Scrolling a playing video off screen counts too (Settings, under
  /// Auto Picture in Picture).
  static let scrollKey = "AutoPictureInPictureScroll"
  static var scrollIsOn: Bool { isOn && (UserDefaults.standard.object(forKey: scrollKey) as? Bool ?? true) }

  @MainActor static func enter(_ view: WKWebView?, scroll: Bool = false) {
    guard isOn, let view else { return }
    view.evaluateJavaScript(enterSource(scroll: scroll), in: nil, in: world) { result in
      if case .success(let outcome) = result, outcome as? String != "entered" { unpark(view) }
      let line: String
      switch result {
      case .success(let outcome): line = "enter\(scroll ? " (scroll)" : ""): \(outcome)"
      case .failure(let error): line = "enter failed: \(error.localizedDescription)"
      }
      MainActor.assumeIsolated { note(line) }
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

  @MainActor static func park(_ view: WKWebView) {
    // The window the tab is in (SwiftUI drops it on this update), else the
    // key window of the scene in front: with several scenes (iPad, little
    // windows) the first one listed may not be on screen, and a page
    // parked there ends its window at once.
    let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
    let front = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
    guard parking == nil || view.superview !== parking,
      let window = view.window ?? front?.keyWindow ?? front?.windows.first
    else {
      note("park: no window")
      return
    }
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
    note("parked the tab's view")
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
