// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SIDEBAR_NOW_PLAYING_TRACKER_H_
#define COMPASS_BROWSER_UI_WEBUI_SIDEBAR_NOW_PLAYING_TRACKER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/media_session/public/mojom/audio_focus.mojom.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}

namespace compass {

// The media a window's sidebar shows "now playing" widgets for, as Arc does
// (docs/design/sidebar.md, "Now playing"): the profile's tabs with a media
// session, playing or paused, that are in the window but not shown in it, or
// parked (another space's), and whose video isn't in picture-in-picture.
// Finds media sessions through the audio focus manager, so a tab only gets
// watched once it plays, and follows each until its contents go.
class NowPlayingTracker : public media_session::mojom::AudioFocusObserver {
 public:
  struct Item {
    // The session's audio focus request ID, as a string.
    std::string id;
    // The session's title (the page's, unless the site sets one).
    std::u16string title;
    // The tab's page, for its favicon.
    GURL url;
    bool playing = false;
    bool muted = false;
    // False while it's muted: muting takes the players out of the session.
    bool can_play_pause = false;
    bool can_previous = false;
    bool can_next = false;
    bool can_picture_in_picture = false;

    friend bool operator==(const Item&, const Item&) = default;
  };

  enum class Action {
    kPlay,
    kPause,
    kPreviousTrack,
    kNextTrack,
    kPictureInPicture,
    kMute,
    kUnmute,
    // Pauses and hides the widget until the media plays again.
    kDismiss,
    // Shows the tab: activates it in this window, or reveals a parked one.
    kShowTab,
  };

  // How long a session must be eligible before it shows: leaving a tab whose
  // video goes to auto picture-in-picture would otherwise flash a widget.
  static constexpr base::TimeDelta kShowDelay = base::Milliseconds(800);

  // |on_changed| runs whenever GetItems() changes.
  NowPlayingTracker(BrowserWindowInterface* browser,
                    base::RepeatingClosure on_changed);
  NowPlayingTracker(const NowPlayingTracker&) = delete;
  NowPlayingTracker& operator=(const NowPlayingTracker&) = delete;
  ~NowPlayingTracker() override;

  // In the order they first showed.
  const std::vector<Item>& items() const { return items_; }

  // Ignored for an ID that isn't shown.
  void Act(const std::string& id, Action action);

  // media_session::mojom::AudioFocusObserver:
  void OnFocusGained(
      media_session::mojom::AudioFocusRequestStatePtr state) override;
  void OnFocusLost(
      media_session::mojom::AudioFocusRequestStatePtr state) override;
  void OnRequestIdReleased(const base::UnguessableToken& request_id) override;

 private:
  class Session;

  void OnFocusRequests(
      std::vector<media_session::mojom::AudioFocusRequestStatePtr> requests);
  void Track(const base::UnguessableToken& request_id);
  // Recomputes the items soon, once the change at hand (a tab moving, a
  // session updating) has settled.
  void ScheduleUpdate();
  void Update();
  // Whether |session|'s tab is one this window shows a widget for, leaving
  // out its dismissal and the show delay.
  bool IsEligible(Session& session) const;
  Session* FindShown(const std::string& id);

  const raw_ptr<BrowserWindowInterface> browser_;
  const base::RepeatingClosure on_changed_;
  mojo::Remote<media_session::mojom::AudioFocusManager> focus_manager_;
  mojo::Receiver<media_session::mojom::AudioFocusObserver> observer_receiver_{
      this};
  std::map<base::UnguessableToken, std::unique_ptr<Session>> sessions_;
  // When each shown session first showed, for their order.
  std::vector<base::UnguessableToken> shown_order_;
  std::vector<Item> items_;
  bool update_pending_ = false;
  // For sessions waiting out kShowDelay.
  base::OneShotTimer show_timer_;

  base::WeakPtrFactory<NowPlayingTracker> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SIDEBAR_NOW_PLAYING_TRACKER_H_
