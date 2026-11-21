// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_LIVE_FOLDERS_LIVE_PLUGIN_MANIFEST_H_
#define COMPASS_BROWSER_LIVE_FOLDERS_LIVE_PLUGIN_MANIFEST_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"

class GURL;

namespace compass {

// A live folder plugin's manifest, validated. The C++ port of
// plugins/runtime/manifest.js, which is the reference: the rules and the
// conformance cases are shared (docs/design/live-folders.md, "Manifest").
// The browser enforces them itself, since nothing in the plugin's renderer is
// trusted.
struct LivePluginManifest {
  struct SelectOption {
    std::string value;
    std::string label;
  };

  struct ConfigField {
    enum class Type { kString, kUrl, kNumber, kBool, kSelect };

    ConfigField();
    ConfigField(const ConfigField&);
    ConfigField& operator=(const ConfigField&);
    ConfigField(ConfigField&&);
    ConfigField& operator=(ConfigField&&);
    ~ConfigField();

    std::string key;
    Type type = Type::kString;
    std::string label;
    // As the manifest gives it; checked when a config is.
    std::optional<base::Value> default_value;
    bool optional = false;
    std::string placeholder;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<SelectOption> options;
  };

  struct SecretField {
    std::string key;
    std::string label;
    std::string help;
    bool optional = false;
  };

  // A tab preview's selector group (docs/design/live-folders.md, "Tab
  // previews"): what the page agent reads from each element |selector|
  // matches, at most |max| of them.
  struct SelectGroup {
    SelectGroup();
    SelectGroup(const SelectGroup&);
    SelectGroup& operator=(const SelectGroup&);
    SelectGroup(SelectGroup&&);
    SelectGroup& operator=(SelectGroup&&);
    ~SelectGroup();

    std::string selector;
    std::vector<std::string> attrs;
    bool text = false;
    // Name → "selector", "selector@attribute" or "@attribute".
    std::map<std::string, std::string, std::less<>> fields;
    int max = 50;
  };

  // A tab preview's advanced mode: requests with the tab's session.
  struct PreviewSession {
    PreviewSession();
    PreviewSession(const PreviewSession&);
    PreviewSession& operator=(const PreviewSession&);
    PreviewSession(PreviewSession&&);
    PreviewSession& operator=(PreviewSession&&);
    ~PreviewSession();

    // The registrable domain every match is on.
    std::string site;
    // Narrows the site to these host patterns, when not empty.
    std::vector<std::string> hosts;
    // Cross-site host patterns, reached without cookies.
    std::vector<std::string> related;
    // Non-HttpOnly cookies of the site the plugin may read.
    std::vector<std::string> page_cookies;
  };

  struct Preview {
    Preview();
    Preview(const Preview&);
    Preview& operator=(const Preview&);
    Preview(Preview&&);
    Preview& operator=(Preview&&);
    ~Preview();

    // "https://host/path*" patterns.
    std::vector<std::string> matches;
    std::map<std::string, SelectGroup, std::less<>> select;
    bool page_text = false;
    // Its result's count shows on the tab's row, kept current in the
    // background.
    bool badge = false;
    std::optional<PreviewSession> session;
    std::vector<ConfigField> config;
  };

  // No plugin refreshes more often than this.
  static constexpr base::TimeDelta kRefreshFloor = base::Seconds(300);
  static constexpr base::TimeDelta kDefaultRefresh = base::Seconds(900);
  static constexpr int kApiVersion = 1;

  LivePluginManifest();
  LivePluginManifest(const LivePluginManifest&);
  LivePluginManifest& operator=(const LivePluginManifest&);
  LivePluginManifest(LivePluginManifest&&);
  LivePluginManifest& operator=(LivePluginManifest&&);
  ~LivePluginManifest();

  std::string id;
  std::string name;
  std::string version;
  std::string description;
  // A file name in the plugin's folder.
  std::string entry;
  // A file name in the plugin's folder, or empty.
  std::string icon;
  // Host patterns: "example.com", "*.example.com", "{config:key}",
  // "{secret:key}". Cookies are a subset of hosts.
  std::vector<std::string> hosts;
  std::vector<std::string> cookies;
  std::vector<SecretField> secrets;
  std::vector<ConfigField> config;
  base::TimeDelta refresh_default = kDefaultRefresh;
  base::TimeDelta refresh_min = kRefreshFloor;
  // Whether it fills live folders ("folder": false opts out).
  bool folder = true;
  // Whether, and on which pages, it shows tab previews.
  std::optional<Preview> preview;

  const ConfigField* FindConfigField(std::string_view key) const;
};

// Every problem found, as messages for the developer.
using LivePluginErrors = std::vector<std::string>;

base::expected<LivePluginManifest, LivePluginErrors> ParseLivePluginManifest(
    const base::Value& raw);

// |config| checked against |manifest|'s fields, with defaults filled in and
// unknown keys dropped (validateConfig()). Errors name fields by label.
base::expected<base::DictValue, LivePluginErrors> ValidateLiveConfig(
    const LivePluginManifest& manifest,
    const base::DictValue& config);

// The same against other fields, such as a preview's.
base::expected<base::DictValue, LivePluginErrors> ValidateLiveConfigFields(
    const std::vector<LivePluginManifest::ConfigField>& fields,
    const base::DictValue& config);

// The registrable domain (eTLD+1, with the Public Suffix List's private
// registries, so each github.io site is its own) of |host|, or empty for a
// public suffix or an IP address.
std::string LiveSiteOf(std::string_view host);

// Whether |url| is one of |preview|'s matches.
bool LivePreviewMatches(const LivePluginManifest::Preview& preview,
                        const GURL& url);

// A plugin's secrets by key.
using LiveSecrets = std::map<std::string, std::string, std::less<>>;

// The plain host patterns a refresh may reach and send cookies to, with
// placeholders resolved against the validated config and the secrets
// (resolveHosts()).
struct LiveAllowedHosts {
  LiveAllowedHosts();
  LiveAllowedHosts(const LiveAllowedHosts&);
  LiveAllowedHosts& operator=(const LiveAllowedHosts&);
  ~LiveAllowedHosts();

  std::vector<std::string> hosts;
  std::vector<std::string> cookies;
};
LiveAllowedHosts ResolveLiveHosts(const LivePluginManifest& manifest,
                                  const base::DictValue& config,
                                  const LiveSecrets& secrets);

// Whether |host| matches |pattern| ("example.com" or "*.example.com", the
// domain and its subdomains).
bool LiveHostMatches(std::string_view pattern, std::string_view host);

// Whether a plugin may fetch |url|: http(s), no user info, on a host one of
// |patterns| matches.
bool LiveUrlAllowed(const GURL& url, const std::vector<std::string>& patterns);

}  // namespace compass

#endif  // COMPASS_BROWSER_LIVE_FOLDERS_LIVE_PLUGIN_MANIFEST_H_
