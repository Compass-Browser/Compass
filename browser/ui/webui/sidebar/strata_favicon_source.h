// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_SIDEBAR_STRATA_FAVICON_SOURCE_H_
#define STRATA_BROWSER_UI_WEBUI_SIDEBAR_STRATA_FAVICON_SOURCE_H_

#include <memory>
#include <string>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "content/public/browser/url_data_source.h"

class FaviconSource;
class Profile;

namespace strata {

inline constexpr char kStrataFaviconHost[] = "strata-favicon";

// chrome://strata-favicon/?profile=<id>&<chrome://favicon2 parameters>: the
// icon in profile <id> (a directory name, as the sidebar's ProfileOption IDs),
// or in the page's own profile without one or if it isn't loaded. The sidebar
// shows other profiles' entries (their spaces' previews, and the window's own
// after a switch, which keeps the page), whose icons chrome://favicon2 only
// finds in the page's profile.
class StrataFaviconSource : public content::URLDataSource,
                            public ProfileObserver {
 public:
  explicit StrataFaviconSource(Profile* profile);
  StrataFaviconSource(const StrataFaviconSource&) = delete;
  StrataFaviconSource& operator=(const StrataFaviconSource&) = delete;
  ~StrataFaviconSource() override;

  // content::URLDataSource:
  std::string GetSource() override;
  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& wc_getter,
                        GotDataCallback callback) override;
  std::string GetMimeType(const GURL& url) override;
  bool AllowCaching() override;
  bool ShouldReplaceExistingSource() override;

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

 private:
  // |profile|'s chrome://favicon2, which serves the requests for it.
  FaviconSource& GetSourceFor(Profile* profile);

  const raw_ptr<Profile> profile_;
  base::flat_map<Profile*, std::unique_ptr<FaviconSource>> sources_;
  base::ScopedMultiSourceObservation<Profile, ProfileObserver>
      profile_observations_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_SIDEBAR_STRATA_FAVICON_SOURCE_H_
