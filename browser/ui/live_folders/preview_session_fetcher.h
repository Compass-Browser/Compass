// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_PREVIEW_SESSION_FETCHER_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_PREVIEW_SESSION_FETCHER_H_

#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/live_folders/live_fetcher.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
}

namespace compass {

// Where a tab preview's request may go (docs/design/live-folders.md, "Tab
// previews"): the tab's own site, subdomains included and narrowed by
// session.hosts, with the session's cookies; a declared related host,
// without them; or nowhere.
enum class PreviewHostKind { kRefused, kOwnSite, kRelated };

PreviewHostKind ClassifyPreviewUrl(const LivePluginManifest::Preview& preview,
                                   const GURL& tab_url,
                                   const GURL& url);

// Whether a preview item's icon may load: the tab's site, and with the
// session granted, the related hosts the user saw when granting it. Loading
// an image is a request, so it could carry page data anywhere else.
bool PreviewIconAllowed(const LivePluginManifest::Preview& preview,
                        const GURL& tab_url,
                        bool session_granted,
                        const GURL& icon);

// Sends an own-site request from the hovered tab itself, in an isolated world,
// so it carries the page's origin and cookies as the site's own requests do.
class PreviewPageTransport {
 public:
  struct Response {
    Response();
    Response(Response&&);
    Response& operator=(Response&&);
    ~Response();

    // The response was a redirect, which comes back opaque: the browser
    // sends the request again and checks each hop.
    bool redirect = false;
    LiveFetchResponse response;
  };
  using Callback =
      base::OnceCallback<void(base::expected<Response, LiveError>)>;

  virtual ~PreviewPageTransport() = default;
  virtual void Fetch(const LiveFetchRequest& request,
                     size_t max_response_bytes,
                     Callback callback) = 0;
};

// compass.fetch() for a tab preview whose plugin may use the tab's session.
// Same rules as tools/live_folders/runtime.mjs, runPreview().
class PreviewSessionFetcher : public LiveFetchClient {
 public:
  struct Limits {
    size_t max_requests = 5;
    size_t max_response_bytes = 2 * 1024 * 1024;
    int max_redirects = 5;
  };

  // |page| sends own-site requests while the tab is live; null for an
  // unloaded or discarded tab, whose requests all go from the browser.
  PreviewSessionFetcher(
      LivePluginManifest::Preview preview,
      GURL tab_url,
      scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
      std::unique_ptr<PreviewPageTransport> page,
      Limits limits);
  PreviewSessionFetcher(const PreviewSessionFetcher&) = delete;
  PreviewSessionFetcher& operator=(const PreviewSessionFetcher&) = delete;
  ~PreviewSessionFetcher() override;

  // LiveFetchClient:
  void Fetch(const LiveFetchRequest& request, Callback callback) override;
  const std::vector<LiveFetchRecord>& fetches() const override;

 private:
  void FromBrowser(LiveFetchRequest request, Callback callback);
  void OnFromPage(LiveFetchRequest request,
                  Callback callback,
                  base::expected<PreviewPageTransport::Response, LiveError>
                      result);
  void OnFromBrowser(LiveFetchRequest request,
                     Callback callback,
                     base::expected<LiveFetchResponse, LiveError> result);

  const LivePluginManifest::Preview preview_;
  const GURL tab_url_;
  const Limits limits_;
  std::unique_ptr<PreviewPageTransport> page_;
  // Own-site hosts with cookies, related hosts without; hops checked.
  std::unique_ptr<LiveFetcher> browser_;
  size_t requests_ = 0;
  std::vector<LiveFetchRecord> records_;
  base::WeakPtrFactory<PreviewSessionFetcher> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_PREVIEW_SESSION_FETCHER_H_
