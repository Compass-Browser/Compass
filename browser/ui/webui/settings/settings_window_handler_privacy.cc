// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's Privacy pane (phase 3, docs/design/settings-window.md):
// site settings (the permissions' defaults, the sites with permissions of
// their own, one site's), clearing browsing data and Safe Browsing, as
// upstream's SiteSettingsHandler and ClearBrowsingDataHandler do them for
// chrome://settings.

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/browsing_data/chrome_browsing_data_remover_constants.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/settings/site_settings_helper.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "components/browsing_data/core/browsing_data_utils.h"
#include "components/content_settings/core/browser/content_settings_info.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browsing_data_filter_builder.h"
#include "content/public/browser/browsing_data_remover.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace compass {

namespace {

using sidebar::mojom::PermissionValue;

PermissionValue ToMojo(ContentSetting setting) {
  switch (setting) {
    case CONTENT_SETTING_DEFAULT:
      return PermissionValue::kDefault;
    case CONTENT_SETTING_BLOCK:
      return PermissionValue::kBlock;
    case CONTENT_SETTING_ASK:
      return PermissionValue::kAsk;
    default:
      // Allow, and its variants (session only, important content).
      return PermissionValue::kAllow;
  }
}

ContentSetting FromMojo(PermissionValue value) {
  switch (value) {
    case PermissionValue::kDefault:
      return CONTENT_SETTING_DEFAULT;
    case PermissionValue::kAllow:
      return CONTENT_SETTING_ALLOW;
    case PermissionValue::kBlock:
      return CONTENT_SETTING_BLOCK;
    case PermissionValue::kAsk:
      return CONTENT_SETTING_ASK;
  }
  return CONTENT_SETTING_DEFAULT;
}

// The registry's description of |type|, if it's a content setting (the
// window doesn't show other website settings).
const content_settings::ContentSettingsInfo* InfoFor(ContentSettingsType type) {
  return content_settings::ContentSettingsRegistry::GetInstance()->Get(type);
}

// The values |type| can take: for its default, or for a site.
std::vector<PermissionValue> ChoicesFor(ContentSettingsType type,
                                        bool for_default) {
  std::vector<PermissionValue> choices;
  const content_settings::ContentSettingsInfo* info = InfoFor(type);
  if (!info) {
    return choices;
  }
  for (ContentSetting setting :
       {CONTENT_SETTING_ASK, CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK}) {
    if (for_default ? info->IsDefaultSettingValid(setting)
                    : info->IsSettingValid(setting)) {
      choices.push_back(ToMojo(setting));
    }
  }
  return choices;
}

// The permissions the window shows, as upstream's site settings list them.
std::vector<ContentSettingsType> ShownTypes(Profile* profile,
                                            const std::string& origin) {
  std::vector<ContentSettingsType> types;
  for (ContentSettingsType type :
       site_settings::GetVisiblePermissionCategories(origin, profile)) {
    if (InfoFor(type) && site_settings::HasRegisteredGroupName(type) &&
        ChoicesFor(type, /*for_default=*/origin.empty()).size() > 1) {
      types.push_back(type);
    }
  }
  return types;
}

std::optional<ContentSettingsType> TypeFromName(const std::string& name) {
  ContentSettingsType type =
      site_settings::ContentSettingsTypeFromGroupName(name);
  if (type == ContentSettingsType::DEFAULT || !InfoFor(type)) {
    return std::nullopt;
  }
  return type;
}

// A site's URL from the origin the page has; invalid if it isn't a web
// origin.
GURL SiteURL(const std::string& origin) {
  GURL url(origin);
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() ? url : GURL();
}

// The observer of one removal (RemoveAndReply(), which wants it registered
// first): calls |callback| once it's done, and deletes itself.
class RemovalWaiter : public content::BrowsingDataRemover::Observer {
 public:
  RemovalWaiter(content::BrowsingDataRemover* remover,
                base::OnceClosure callback)
      : remover_(remover), callback_(std::move(callback)) {
    remover_->AddObserver(this);
  }

  void OnBrowsingDataRemoverDone(uint64_t failed_data_types) override {
    remover_->RemoveObserver(this);
    std::move(callback_).Run();
    delete this;
  }

 private:
  ~RemovalWaiter() override = default;

  raw_ptr<content::BrowsingDataRemover> remover_;
  base::OnceClosure callback_;
};

}  // namespace

// Site settings ---------------------------------------------------------------

void SettingsWindowHandler::GetPermissionDefaults(
    GetPermissionDefaultsCallback callback) {
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  std::vector<sidebar::mojom::PermissionDefaultPtr> defaults;
  for (ContentSettingsType type : ShownTypes(profile_, std::string())) {
    content_settings::ProviderType provider;
    const ContentSetting setting =
        map->GetDefaultContentSetting(type, &provider);
    defaults.push_back(sidebar::mojom::PermissionDefault::New(
        std::string(site_settings::ContentSettingsTypeToGroupName(type)),
        ToMojo(setting), ChoicesFor(type, /*for_default=*/true),
        provider != content_settings::ProviderType::kDefaultProvider &&
            provider != content_settings::ProviderType::kPrefProvider));
  }
  std::move(callback).Run(std::move(defaults));
}

void SettingsWindowHandler::SetPermissionDefault(const std::string& type_name,
                                                 PermissionValue value) {
  std::optional<ContentSettingsType> type = TypeFromName(type_name);
  const ContentSetting setting = FromMojo(value);
  if (!type || setting == CONTENT_SETTING_DEFAULT ||
      !InfoFor(*type)->IsDefaultSettingValid(setting)) {
    return;
  }
  HostContentSettingsMapFactory::GetForProfile(profile_)
      ->SetDefaultContentSetting(*type, setting);
}

void SettingsWindowHandler::GetSites(GetSitesCallback callback) {
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  // Each site's origin and how many permissions of its own it has.
  std::map<std::string, int> counts;
  for (ContentSettingsType type : ShownTypes(profile_, std::string())) {
    for (const ContentSettingPatternSource& entry :
         site_settings::GetSingleOriginExceptionsForContentType(map, type)) {
      if (entry.source != content_settings::ProviderType::kPrefProvider) {
        continue;
      }
      GURL url = entry.primary_pattern.ToRepresentativeUrl();
      if (url.is_valid() && url.SchemeIsHTTPOrHTTPS()) {
        counts[url::Origin::Create(url).Serialize()]++;
      }
    }
  }
  std::vector<sidebar::mojom::SiteSummaryPtr> sites;
  for (const auto& [origin, count] : counts) {
    sites.push_back(sidebar::mojom::SiteSummary::New(
        origin,
        site_settings::GetDisplayNameForGURL(profile_, GURL(origin),
                                             /*hostname_only=*/true),
        count));
  }
  std::move(callback).Run(std::move(sites));
}

void SettingsWindowHandler::GetSitePermissions(
    const std::string& origin,
    GetSitePermissionsCallback callback) {
  const GURL url = SiteURL(origin);
  if (!url.is_valid()) {
    std::move(callback).Run(std::string(), {});
    return;
  }
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  std::vector<sidebar::mojom::SitePermissionPtr> permissions;
  for (ContentSettingsType type : ShownTypes(profile_, origin)) {
    site_settings::SiteSettingSource source;
    const ContentSetting effective = site_settings::GetContentSettingForOrigin(
        profile_, map, url, type, &source);
    // The site's own setting: one the user made for it.
    content_settings::SettingInfo info;
    map->GetWebsiteSetting(url, url, type, &info);
    const bool own = info.source == content_settings::SettingSource::kUser &&
                     !info.primary_pattern.MatchesAllHosts();
    permissions.push_back(sidebar::mojom::SitePermission::New(
        std::string(site_settings::ContentSettingsTypeToGroupName(type)),
        own ? ToMojo(effective) : PermissionValue::kDefault, ToMojo(effective),
        ChoicesFor(type, /*for_default=*/false),
        source == site_settings::SiteSettingSource::kPolicy ||
            source == site_settings::SiteSettingSource::kExtension));
  }
  std::move(callback).Run(
      site_settings::GetDisplayNameForGURL(profile_, url,
                                           /*hostname_only=*/true),
      std::move(permissions));
}

void SettingsWindowHandler::SetSitePermission(const std::string& origin,
                                              const std::string& type_name,
                                              PermissionValue value) {
  const GURL url = SiteURL(origin);
  std::optional<ContentSettingsType> type = TypeFromName(type_name);
  const ContentSetting setting = FromMojo(value);
  if (!url.is_valid() || !type ||
      (setting != CONTENT_SETTING_DEFAULT &&
       !InfoFor(*type)->IsSettingValid(setting))) {
    return;
  }
  HostContentSettingsMapFactory::GetForProfile(profile_)
      ->SetContentSettingDefaultScope(url, url, *type, setting);
}

void SettingsWindowHandler::ResetSitePermissions(const std::string& origin) {
  const GURL url = SiteURL(origin);
  if (!url.is_valid()) {
    return;
  }
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  for (ContentSettingsType type : ShownTypes(profile_, origin)) {
    map->SetContentSettingDefaultScope(url, url, type, CONTENT_SETTING_DEFAULT);
  }
}

void SettingsWindowHandler::OnContentSettingChanged(
    const ContentSettingsPattern& primary_pattern,
    const ContentSettingsPattern& secondary_pattern,
    ContentSettingsTypeSet content_type_set) {
  page_->ContentSettingsChanged();
}

// Clearing data ---------------------------------------------------------------

void SettingsWindowHandler::ClearSiteData(const std::string& origin,
                                          ClearSiteDataCallback callback) {
  const GURL url = SiteURL(origin);
  if (!url.is_valid()) {
    std::move(callback).Run();
    return;
  }
  content::BrowsingDataRemover* remover = profile_->GetBrowsingDataRemover();
  auto filter = content::BrowsingDataFilterBuilder::Create(
      content::BrowsingDataFilterBuilder::Mode::kDelete);
  filter->AddOrigin(url::Origin::Create(url));
  // Deletes itself once done.
  auto* waiter = new RemovalWaiter(remover, std::move(callback));
  remover->RemoveWithFilterAndReply(
      base::Time(), base::Time::Max(),
      chrome_browsing_data_remover::DATA_TYPE_SITE_DATA |
          content::BrowsingDataRemover::DATA_TYPE_CACHE,
      chrome_browsing_data_remover::ALL_ORIGIN_TYPES, std::move(filter),
      waiter);
}

void SettingsWindowHandler::ClearBrowsingData(
    sidebar::mojom::ClearBrowsingDataOptionsPtr options,
    ClearBrowsingDataCallback callback) {
  uint64_t mask = 0;
  if (options->history) {
    mask |= chrome_browsing_data_remover::DATA_TYPE_HISTORY;
  }
  if (options->downloads) {
    mask |= content::BrowsingDataRemover::DATA_TYPE_DOWNLOADS;
  }
  if (options->cookies) {
    mask |= chrome_browsing_data_remover::DATA_TYPE_SITE_DATA;
  }
  if (options->cache) {
    mask |= content::BrowsingDataRemover::DATA_TYPE_CACHE;
  }
  if (options->passwords) {
    mask |= chrome_browsing_data_remover::DATA_TYPE_PASSWORDS;
  }
  if (options->form_data) {
    mask |= chrome_browsing_data_remover::DATA_TYPE_FORM_DATA;
  }
  if (options->site_settings) {
    mask |= chrome_browsing_data_remover::DATA_TYPE_CONTENT_SETTINGS;
  }
  const int period = options->time_period;
  if (!mask || period < 0 ||
      period > static_cast<int>(browsing_data::TimePeriod::TIME_PERIOD_LAST)) {
    std::move(callback).Run();
    return;
  }
  const base::Time begin = browsing_data::CalculateBeginDeleteTime(
      static_cast<browsing_data::TimePeriod>(period));
  content::BrowsingDataRemover* remover = profile_->GetBrowsingDataRemover();
  // Deletes itself once done.
  auto* waiter = new RemovalWaiter(remover, std::move(callback));
  remover->RemoveAndReply(
      begin, base::Time::Max(), mask,
      content::BrowsingDataRemover::ORIGIN_TYPE_UNPROTECTED_WEB, waiter);
}

// Security
// ---------------------------------------------------------------------

void SettingsWindowHandler::SetSafeBrowsingLevel(int32_t level) {
  if (level < 0 ||
      level > static_cast<int>(safe_browsing::SafeBrowsingState::kMaxValue)) {
    return;
  }
  PrefService* prefs = profile_->GetPrefs();
  if (prefs->IsManagedPreference(::prefs::kSafeBrowsingEnabled) ||
      prefs->IsManagedPreference(::prefs::kSafeBrowsingEnhanced)) {
    return;
  }
  safe_browsing::SetSafeBrowsingState(
      prefs, static_cast<safe_browsing::SafeBrowsingState>(level));
}

}  // namespace compass
