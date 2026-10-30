// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/strata_favicon_source.h"

#include <utility>

#include "base/files/file_path.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/webui/favicon_source.h"
#include "chrome/common/webui_url_constants.h"
#include "components/favicon_base/favicon_url_parser.h"
#include "content/public/common/url_constants.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace strata {

namespace {

constexpr char kProfileParam[] = "profile";

// The loaded profile in directory |id|, or null.
Profile* FindProfile(const std::string& id) {
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  const base::FilePath name = base::FilePath::FromUTF8Unsafe(id);
  if (!profile_manager || id.empty() || name.BaseName() != name) {
    return nullptr;
  }
  return profile_manager->GetProfileByPath(
      profile_manager->user_data_dir().Append(name));
}

}  // namespace

StrataFaviconSource::StrataFaviconSource(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

StrataFaviconSource::~StrataFaviconSource() = default;

std::string StrataFaviconSource::GetSource() {
  return kStrataFaviconHost;
}

void StrataFaviconSource::StartDataRequest(
    const GURL& url,
    const content::WebContents::Getter& wc_getter,
    GotDataCallback callback) {
  std::string id;
  net::GetValueForKeyInQuery(url, kProfileParam, &id);
  Profile* profile = FindProfile(id);
  if (!profile || profile->ShutdownStarted()) {
    profile = profile_;
  }
  // The rest is chrome://favicon2's.
  GURL::Replacements replacements;
  const std::string query =
      net::AppendOrReplaceQueryParameter(url, kProfileParam, std::nullopt)
          .GetQuery();
  replacements.SetQueryStr(query);
  const GURL favicon2_url =
      GURL(std::string(content::kChromeUIScheme) + "://" +
           chrome::kChromeUIFavicon2Host + "/")
          .ReplaceComponents(replacements);
  GetSourceFor(profile).StartDataRequest(favicon2_url, wc_getter,
                                         std::move(callback));
}

std::string StrataFaviconSource::GetMimeType(const GURL& url) {
  return "image/png";
}

bool StrataFaviconSource::AllowCaching() {
  return false;
}

bool StrataFaviconSource::ShouldReplaceExistingSource() {
  // Each sidebar adds one; the first keeps serving.
  return false;
}

void StrataFaviconSource::OnProfileWillBeDestroyed(Profile* profile) {
  profile_observations_.RemoveObservation(profile);
  sources_.erase(profile);
}

FaviconSource& StrataFaviconSource::GetSourceFor(Profile* profile) {
  std::unique_ptr<FaviconSource>& source = sources_[profile];
  if (!source) {
    source = std::make_unique<FaviconSource>(
        profile, chrome::FaviconUrlFormat::kFavicon2);
    if (profile != profile_) {
      profile_observations_.AddObservation(profile);
    }
  }
  return *source;
}

}  // namespace strata
