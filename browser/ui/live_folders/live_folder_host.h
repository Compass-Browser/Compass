// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_HOST_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_HOST_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/live_folders/live_fetcher.h"
#include "compass/browser/ui/webui/live_folders/live_folders.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;

namespace content {
class WebContents;
}

namespace compass {

inline constexpr char kLiveFoldersHost[] = "live-folders";
inline constexpr char kLiveFoldersUrl[] = "chrome-untrusted://live-folders/";

// Runs plugins for a profile: a hidden WebContents on
// chrome-untrusted://live-folders/, which starts a dedicated Worker per
// refresh, and the browser side of its fetches (LiveFetcher). The page is
// made when a refresh needs it and kept; if it goes away (its renderer
// crashed, say) the refreshes in it fail and the next one makes a new page.
class LiveFolderHost : public live_folders::mojom::PageHandler {
 public:
  struct RunSpec {
    RunSpec();
    RunSpec(RunSpec&&);
    RunSpec& operator=(RunSpec&&);
    ~RunSpec();

    // Paths on the page's origin, loaded in order.
    std::vector<std::string> scripts;
    base::DictValue config;
    base::DictValue env;
    base::TimeDelta timeout = base::Seconds(30);
    int max_items = 50;
    // compass.fetch() for this refresh.
    std::unique_ptr<LiveFetcher> fetcher;
  };

  // The normalized result's JSON, or why the refresh failed; and what the
  // plugin logged.
  using RunCallback =
      base::OnceCallback<void(base::expected<std::string, LiveError> result,
                              std::vector<std::string> log)>;

  explicit LiveFolderHost(Profile* profile);
  LiveFolderHost(const LiveFolderHost&) = delete;
  LiveFolderHost& operator=(const LiveFolderHost&) = delete;
  ~LiveFolderHost() override;

  // Starts a refresh and returns its ID.
  int Run(RunSpec spec, RunCallback callback);
  // Stops a refresh; its callback doesn't run.
  void Cancel(int run_id);

  // Whether |contents| is this host's page.
  bool IsHostContents(const content::WebContents* contents) const;

  // From the page's WebUI controller.
  void BindPage(mojo::PendingRemote<live_folders::mojom::Page> page,
                mojo::PendingReceiver<live_folders::mojom::PageHandler> handler);

  content::WebContents* web_contents_for_testing() {
    return web_contents_.get();
  }

  // live_folders::mojom::PageHandler:
  void Fetch(int32_t run_id,
             live_folders::mojom::FetchRequestPtr request,
             FetchCallback callback) override;
  void Log(int32_t run_id, const std::string& message) override;

 private:
  struct Running;

  void EnsurePage();
  void StartOnPage(int run_id);
  void OnRunDone(int run_id, live_folders::mojom::RunResultPtr result);
  void Finish(int run_id, base::expected<std::string, LiveError> result);
  void OnPageGone();

  const raw_ptr<Profile> profile_;
  std::unique_ptr<content::WebContents> web_contents_;
  mojo::Remote<live_folders::mojom::Page> page_;
  mojo::Receiver<live_folders::mojom::PageHandler> receiver_{this};
  int next_run_id_ = 1;
  std::map<int, std::unique_ptr<Running>> runs_;
  base::WeakPtrFactory<LiveFolderHost> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_HOST_H_
