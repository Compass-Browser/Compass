// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/now_playing_tracker.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/media_session_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "compass/browser/window_sync/window_features.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_session_service.h"
#include "services/media_session/public/mojom/media_session.mojom.h"

namespace compass {

using media_session::mojom::MediaSessionAction;
using media_session::mojom::MediaSessionInfo;

// One media session: its tab's state, as the session and the tab report it.
class NowPlayingTracker::Session
    : public media_session::mojom::MediaSessionObserver,
      public content::WebContentsObserver {
 public:
  Session(content::WebContents* contents, base::RepeatingClosure on_changed)
      : content::WebContentsObserver(contents),
        on_changed_(std::move(on_changed)) {
    content::MediaSession::Get(contents)->AddObserver(
        receiver_.BindNewPipeAndPassRemote());
    WatchTab();
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() override = default;

  content::WebContents* contents() const { return web_contents(); }
  // Muting a tab takes its players out of its session, which goes inactive:
  // its widget stays, as it was before, and its speaker can unmute it. Only
  // the speaker works then.
  bool muted_out() const {
    return last_live_ && !IsLive(info_.get()) && web_contents() &&
           web_contents()->IsAudioMuted();
  }
  // The session as the widget shows it.
  const media_session::mojom::MediaSessionInfo* info() const {
    return muted_out() ? last_live_.get() : info_.get();
  }
  const std::u16string& title() const { return title_; }
  bool playing() const { return IsPlaying(info()); }
  bool Can(MediaSessionAction action) const {
    return actions_.contains(action);
  }

  // Shows only once it plays again.
  bool dismissed = false;
  // When it last became eligible; null while it isn't.
  base::TimeTicks eligible_since;

  // media_session::mojom::MediaSessionObserver:
  void MediaSessionInfoChanged(
      media_session::mojom::MediaSessionInfoPtr info) override {
    const bool was_playing = IsPlaying(info_.get());
    info_ = std::move(info);
    if (IsLive(info_.get())) {
      last_live_ = info_.Clone();
    }
    // Plays again: a reply to the dismissal's pause still on its way while it
    // plays doesn't count.
    if (IsPlaying(info_.get()) && !was_playing) {
      dismissed = false;
    }
    on_changed_.Run();
  }
  void MediaSessionMetadataChanged(
      const std::optional<media_session::MediaMetadata>& metadata) override {
    title_ = metadata ? metadata->title : std::u16string();
    on_changed_.Run();
  }
  void MediaSessionActionsChanged(
      const std::vector<MediaSessionAction>& actions) override {
    actions_ = base::flat_set<MediaSessionAction>(actions);
    on_changed_.Run();
  }
  void MediaSessionImagesChanged(
      const base::flat_map<media_session::mojom::MediaSessionImageType,
                           std::vector<media_session::MediaImage>>& images)
      override {}
  void MediaSessionPositionChanged(
      const std::optional<media_session::MediaPosition>& position) override {}

  // content::WebContentsObserver:
  void DidUpdateAudioMutingState(bool muted) override { on_changed_.Run(); }
  void TitleWasSet(content::NavigationEntry* entry) override {
    on_changed_.Run();
  }
  void WebContentsDestroyed() override {
    tab_subscriptions_.clear();
    on_changed_.Run();
  }

 private:
  static bool IsLive(const media_session::mojom::MediaSessionInfo* info) {
    return info && info->is_controllable &&
           info->state != MediaSessionInfo::SessionState::kInactive;
  }
  static bool IsPlaying(const media_session::mojom::MediaSessionInfo* info) {
    return info && info->playback_state ==
                       media_session::mojom::MediaPlaybackState::kPlaying;
  }

  // Follows the tab moving between windows and being shown or hidden. A tab
  // keeps its interface across moves, so this is done once.
  void WatchTab() {
    tabs::TabInterface* tab =
        tabs::TabInterface::MaybeGetFromContents(web_contents());
    if (!tab) {
      return;
    }
    auto changed = [](base::RepeatingClosure on_changed, tabs::TabInterface*) {
      on_changed.Run();
    };
    tab_subscriptions_.push_back(
        tab->RegisterDidBecomeVisible(base::BindRepeating(changed, on_changed_)));
    tab_subscriptions_.push_back(tab->RegisterWillBecomeHidden(
        base::BindRepeating(changed, on_changed_)));
    tab_subscriptions_.push_back(
        tab->RegisterDidInsert(base::BindRepeating(changed, on_changed_)));
    tab_subscriptions_.push_back(tab->RegisterWillDetach(base::BindRepeating(
        [](base::RepeatingClosure on_changed, tabs::TabInterface*,
           tabs::TabInterface::DetachReason) { on_changed.Run(); },
        on_changed_)));
  }

  const base::RepeatingClosure on_changed_;
  mojo::Receiver<media_session::mojom::MediaSessionObserver> receiver_{this};
  media_session::mojom::MediaSessionInfoPtr info_;
  // The last info with players, for muted_out().
  media_session::mojom::MediaSessionInfoPtr last_live_;
  std::u16string title_;
  base::flat_set<MediaSessionAction> actions_;
  std::vector<base::CallbackListSubscription> tab_subscriptions_;
};

NowPlayingTracker::NowPlayingTracker(BrowserWindowInterface* browser,
                                     base::RepeatingClosure on_changed)
    : browser_(browser), on_changed_(std::move(on_changed)) {
  content::GetMediaSessionService().BindAudioFocusManager(
      focus_manager_.BindNewPipeAndPassReceiver());
  focus_manager_->AddObserver(observer_receiver_.BindNewPipeAndPassRemote());
  focus_manager_->GetFocusRequests(base::BindOnce(
      &NowPlayingTracker::OnFocusRequests, weak_factory_.GetWeakPtr()));
}

NowPlayingTracker::~NowPlayingTracker() = default;

void NowPlayingTracker::Act(const std::string& id, Action action) {
  Session* session = FindShown(id);
  if (!session || !session->contents()) {
    return;
  }
  content::WebContents* contents = session->contents();
  content::MediaSession* media = content::MediaSession::Get(contents);
  using SuspendType = media_session::mojom::MediaSession::SuspendType;
  switch (action) {
    case Action::kPlay:
      media->Resume(SuspendType::kUI);
      break;
    case Action::kPause:
      media->Suspend(SuspendType::kUI);
      break;
    case Action::kPreviousTrack:
      media->PreviousTrack();
      break;
    case Action::kNextTrack:
      media->NextTrack();
      break;
    case Action::kPictureInPicture:
      media->EnterPictureInPicture();
      break;
    case Action::kMute:
    case Action::kUnmute:
      SetTabAudioMuted(contents, action == Action::kMute,
                       TabMutedReason::kAudioIndicator, std::string());
      break;
    case Action::kDismiss:
      session->dismissed = true;
      media->Suspend(SuspendType::kUI);
      ScheduleUpdate();
      break;
    case Action::kShowTab: {
      TabStripModel* strip = browser_->GetTabStripModel();
      int index = strip->GetIndexOfWebContents(contents);
      if (index != TabStripModel::kNoTab) {
        strip->ActivateTabAt(index);
      } else {
        RevealParkedTab(contents);
      }
      break;
    }
  }
}

void NowPlayingTracker::OnFocusGained(
    media_session::mojom::AudioFocusRequestStatePtr state) {
  if (state->request_id) {
    Track(*state->request_id);
  }
}

void NowPlayingTracker::OnFocusLost(
    media_session::mojom::AudioFocusRequestStatePtr state) {
  // A session that loses focus to another stays, paused or not: its info
  // says which.
}

void NowPlayingTracker::OnRequestIdReleased(
    const base::UnguessableToken& request_id) {
  // A session paused from outside the page abandons focus too, and must keep
  // its widget: it's followed until its contents go (Update()). The request
  // ID stays the session's, so playing again finds it here.
}

void NowPlayingTracker::OnFocusRequests(
    std::vector<media_session::mojom::AudioFocusRequestStatePtr> requests) {
  for (const auto& request : requests) {
    if (request->request_id) {
      Track(*request->request_id);
    }
  }
}

void NowPlayingTracker::Track(const base::UnguessableToken& request_id) {
  if (sessions_.contains(request_id)) {
    return;
  }
  content::WebContents* contents =
      content::MediaSession::GetWebContentsFromRequestId(request_id);
  // Only this profile's: other profiles' windows show their own.
  if (!contents ||
      contents->GetBrowserContext() != browser_->GetProfile()) {
    return;
  }
  sessions_[request_id] = std::make_unique<Session>(
      contents, base::BindRepeating(&NowPlayingTracker::ScheduleUpdate,
                                    weak_factory_.GetWeakPtr()));
  ScheduleUpdate();
}

void NowPlayingTracker::ScheduleUpdate() {
  if (update_pending_) {
    return;
  }
  update_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&NowPlayingTracker::Update, weak_factory_.GetWeakPtr()));
}

void NowPlayingTracker::Update() {
  update_pending_ = false;
  const base::TimeTicks now = base::TimeTicks::Now();
  std::optional<base::TimeTicks> next_check;
  // Sessions whose tab closed or discarded its contents.
  std::erase_if(sessions_, [](const auto& entry) {
    return !entry.second->contents();
  });
  std::vector<base::UnguessableToken> shown;
  for (auto& [request_id, session] : sessions_) {
    if (!IsEligible(*session)) {
      session->eligible_since = base::TimeTicks();
      continue;
    }
    if (session->eligible_since.is_null()) {
      session->eligible_since = now;
    }
    if (session->dismissed) {
      continue;
    }
    // Shown already, or waited long enough.
    const base::TimeTicks show_at = session->eligible_since + kShowDelay;
    if (std::ranges::contains(shown_order_, request_id) || show_at <= now) {
      shown.push_back(request_id);
    } else if (!next_check || show_at < *next_check) {
      next_check = show_at;
    }
  }

  // Keep the order they first showed in; newcomers go last.
  std::erase_if(shown_order_, [&](const base::UnguessableToken& id) {
    return !std::ranges::contains(shown, id);
  });
  for (const base::UnguessableToken& id : shown) {
    if (!std::ranges::contains(shown_order_, id)) {
      shown_order_.push_back(id);
    }
  }

  std::vector<Item> items;
  for (const base::UnguessableToken& id : shown_order_) {
    Session& session = *sessions_.at(id);
    content::WebContents* contents = session.contents();
    Item item;
    item.id = id.ToString();
    item.title =
        session.title().empty() ? contents->GetTitle() : session.title();
    item.url = contents->GetLastCommittedURL();
    item.playing = session.playing();
    item.muted = contents->IsAudioMuted();
    const bool live = !session.muted_out();
    item.can_play_pause = live;
    item.can_previous =
        live && session.Can(MediaSessionAction::kPreviousTrack);
    item.can_next = live && session.Can(MediaSessionAction::kNextTrack);
    item.can_picture_in_picture =
        live && session.Can(MediaSessionAction::kEnterPictureInPicture);
    items.push_back(std::move(item));
  }

  if (next_check) {
    show_timer_.Start(FROM_HERE, *next_check - now,
                      base::BindOnce(&NowPlayingTracker::Update,
                                     weak_factory_.GetWeakPtr()));
  } else {
    show_timer_.Stop();
  }
  if (items != items_) {
    items_ = std::move(items);
    on_changed_.Run();
  }
}

bool NowPlayingTracker::IsEligible(Session& session) const {
  content::WebContents* contents = session.contents();
  const MediaSessionInfo* info = session.info();
  if (!contents || !info || !info->is_controllable ||
      info->state == MediaSessionInfo::SessionState::kInactive ||
      info->picture_in_picture_state ==
          media_session::mojom::MediaPictureInPictureState::
              kInPictureInPicture) {
    return false;
  }
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab) {
    return false;
  }
  // Another space's tab (or one a profile swap left behind).
  if (IsInParkingWindow(contents)) {
    return true;
  }
  return tab->GetBrowserWindowInterface() == browser_ && !tab->IsVisible();
}

NowPlayingTracker::Session* NowPlayingTracker::FindShown(
    const std::string& id) {
  for (const base::UnguessableToken& request_id : shown_order_) {
    if (request_id.ToString() == id) {
      auto it = sessions_.find(request_id);
      return it == sessions_.end() ? nullptr : it->second.get();
    }
  }
  return nullptr;
}

}  // namespace compass
