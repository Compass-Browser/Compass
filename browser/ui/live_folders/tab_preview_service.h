// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_TAB_PREVIEW_SERVICE_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_TAB_PREVIEW_SERVICE_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "compass/browser/live_folders/live_result.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "url/gurl.h"

class Profile;

namespace content {
class WebContents;
}

namespace compass {

class LiveFolderService;

// What a sidebar hover card shows for a tab a preview plugin matches.
struct TabPreview {
  TabPreview();
  TabPreview(const TabPreview&);
  TabPreview& operator=(const TabPreview&);
  ~TabPreview();

  std::string plugin_id;
  std::u16string plugin_name;
  LivePreviewResult result;
  // When it was made.
  base::Time time;
  // From an earlier visit: the tab isn't loaded, and couldn't say anything
  // newer.
  bool stale = false;
};

// Tab previews (docs/design/live-folders.md, "Tab previews"): which plugin
// previews a URL, by the user's settings (prefs::kTabPreviewPlugins), and
// runs it on a snapshot of the tab taken in an isolated world
// (plugins/runtime/select.js), on the live folders' host. Results are kept
// briefly, so a hover card opens filled, and the last good one per page
// stays for when the tab is unloaded.
// A tab row's badge from a plugin whose manifest has "badge": true.
struct TabCount {
  bool operator==(const TabCount&) const = default;
  bool empty() const { return count <= 0 && !some; }

  int count = 0;
  // Some, without a number (a dot).
  bool some = false;
};

class TabPreviewService {
 public:
  // A fresh result is used again for this long; a run with the tab's session
  // costs requests, so for longer.
  static constexpr base::TimeDelta kFreshFor = base::Seconds(30);
  static constexpr base::TimeDelta kSessionFreshFor = base::Minutes(2);
  // Badges: how often the open tabs are looked at (a new page gets its count
  // within this), and how often each one's plugin runs again.
  static constexpr base::TimeDelta kCountTick = base::Seconds(15);
  static constexpr base::TimeDelta kCountEvery = base::Minutes(1);
  static constexpr base::TimeDelta kSessionCountEvery = base::Minutes(5);

  class Observer : public base::CheckedObserver {
   public:
    // Some tab's badge changed (GetCount()).
    virtual void OnTabCountsChanged() {}
  };

  using Callback = base::OnceCallback<void(std::optional<TabPreview>)>;

  TabPreviewService(Profile* profile, LiveFolderService* service);
  TabPreviewService(const TabPreviewService&) = delete;
  TabPreviewService& operator=(const TabPreviewService&) = delete;
  ~TabPreviewService();

  // The plugin that previews |url|, or null: previews off, no match, or the
  // match turned off.
  const LivePlugin* FindPlugin(const GURL& url) const;

  // A fresh result for |url|, if there is one.
  std::optional<TabPreview> GetCached(const GURL& url) const;

  // Runs the preview for the tab at |url|: from |contents|' page while it's
  // live (null for an unloaded entry). |callback| gets nullopt when there's
  // nothing to show (no plugin, an error, an empty result), and runs at most
  // once even if several asks for the same page overlap.
  void Fetch(content::WebContents* contents, const GURL& url,
             Callback callback);

  // The user's settings for a plugin (Settings › Plugins).
  bool IsEnabled(const LivePlugin& plugin) const;
  void SetEnabled(const std::string& plugin_id, bool enabled);
  // Granted, and for every related host the plugin lists now.
  bool IsSessionGranted(const LivePlugin& plugin) const;
  void SetSessionGranted(const std::string& plugin_id, bool granted);
  base::DictValue GetConfig(const std::string& plugin_id) const;
  void SetConfig(const std::string& plugin_id, base::DictValue config);

  // Drops kept results (settings changed, a plugin was replaced).
  void ClearCache();

  // The badge of the tab showing |url|: none unless a badge plugin counted.
  TabCount GetCount(const GURL& url) const;
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);
  // Runs the badge plugins of the profile's open tabs that are due, and
  // forgets pages no longer open (every kCountTick).
  void RefreshCounts();

  // What the plugin logged on its last preview, for the developer view.
  std::vector<std::string> GetLog(const std::string& plugin_id) const;

  // Runs |plugin|'s preview on |contents|' page as is, without settings or
  // the cache (Claude's test_plugin): {ok, result | error: {code, message},
  // log, requests, snapshot (the start of what previewTab() got)}. With the
  // tab's session if |session|.
  void RunTest(LivePlugin plugin,
               content::WebContents* contents,
               base::DictValue config,
               bool session,
               base::OnceCallback<void(base::DictValue)> callback);

 private:
  struct Entry {
    Entry();
    Entry(const Entry&);
    Entry& operator=(const Entry&);
    ~Entry();

    TabPreview preview;
    base::TimeDelta fresh_for;
  };

  void OnSnapshot(std::string key,
                  std::string plugin_id,
                  base::WeakPtr<content::WebContents> contents,
                  base::Value snapshot);
  void Run(std::string key,
           std::string plugin_id,
           base::WeakPtr<content::WebContents> contents,
           base::DictValue snapshot);
  void OnRun(std::string key,
             std::string plugin_id,
             bool session,
             bool live,
             base::expected<std::string, LiveError> result,
             std::vector<std::string> log);
  void Finish(const std::string& key, std::optional<TabPreview> preview);
  void OnTestSnapshot(LivePlugin plugin,
                      base::WeakPtr<content::WebContents> contents,
                      base::DictValue config,
                      bool session,
                      base::OnceCallback<void(base::DictValue)> callback,
                      base::Value snapshot);
  const base::DictValue* PluginPrefs(const std::string& plugin_id) const;
  void UpdatePluginPrefs(const std::string& plugin_id,
                         base::OnceCallback<void(base::DictValue&)> update);

  const raw_ptr<Profile> profile_;
  const raw_ptr<LiveFolderService> service_;
  // By page URL.
  std::map<std::string, Entry> fresh_;
  std::map<std::string, TabPreview> last_good_;
  std::map<std::string, std::vector<Callback>> pending_;
  std::map<std::string, std::vector<std::string>> logs_;
  // Badges by page URL, and when each page's plugin last ran for one.
  std::map<std::string, TabCount> counts_;
  std::map<std::string, base::Time> counted_;
  base::RepeatingTimer count_timer_;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<TabPreviewService> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_TAB_PREVIEW_SERVICE_H_
