// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_plugin_manifest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "base/containers/contains.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "url/gurl.h"

namespace compass {

namespace {

using ConfigField = LivePluginManifest::ConfigField;

constexpr size_t kMaxIdLength = 100;
constexpr size_t kMaxNameLength = 60;
constexpr size_t kMaxDescriptionLength = 300;

bool IsLowerAlnum(char c) {
  return base::IsAsciiLower(c) || base::IsAsciiDigit(c);
}

// A non-empty string.
const std::string* NonEmptyString(const base::Value* value) {
  const std::string* s = value ? value->GetIfString() : nullptr;
  return s && !s->empty() ? s : nullptr;
}

// ^[a-z0-9]+(\.[a-z0-9][a-z0-9-]*)+$
bool IsPluginId(std::string_view id) {
  std::vector<std::string_view> parts = base::SplitStringPiece(
      id, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() < 2 || parts[0].empty() ||
      !std::ranges::all_of(parts[0], IsLowerAlnum)) {
    return false;
  }
  for (size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].empty() || !IsLowerAlnum(parts[i][0]) ||
        !std::ranges::all_of(parts[i], [](char c) {
          return IsLowerAlnum(c) || c == '-';
        })) {
      return false;
    }
  }
  return true;
}

// ^\d+(\.\d+){0,3}$
bool IsVersion(std::string_view version) {
  std::vector<std::string_view> parts = base::SplitStringPiece(
      version, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  return parts.size() <= 4 &&
         std::ranges::all_of(parts, [](std::string_view part) {
           return !part.empty() &&
                  std::ranges::all_of(part, base::IsAsciiDigit<char>);
         });
}

// ^[A-Za-z][A-Za-z0-9_]*$
bool IsKey(std::string_view key) {
  return !key.empty() && base::IsAsciiAlpha(key[0]) &&
         std::ranges::all_of(key, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '_';
         });
}

// ^[A-Za-z0-9_\-.]+\.(ext|ext...)$
bool IsFileName(std::string_view name,
                std::initializer_list<std::string_view> extensions) {
  if (!std::ranges::all_of(name, [](char c) {
        return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-' ||
               c == '.';
      })) {
    return false;
  }
  return std::ranges::any_of(extensions, [name](std::string_view ext) {
    return name.size() > ext.size() + 1 &&
           name.substr(name.size() - ext.size() - 1) ==
               std::string(".") + std::string(ext);
  });
}

// ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$
bool IsHostLabel(std::string_view label) {
  return !label.empty() && IsLowerAlnum(label.front()) &&
         IsLowerAlnum(label.back()) &&
         std::ranges::all_of(label,
                             [](char c) { return IsLowerAlnum(c) || c == '-'; });
}

// ^(\*\.)?(label\.)*label$
bool IsHostPattern(std::string_view pattern) {
  if (pattern.starts_with("*.")) {
    pattern.remove_prefix(2);
  }
  std::vector<std::string_view> labels = base::SplitStringPiece(
      pattern, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  return std::ranges::all_of(labels, IsHostLabel);
}

struct Placeholder {
  bool config;  // Else a secret.
  std::string_view key;
};

// ^\{(config|secret):key\}$
std::optional<Placeholder> ParsePlaceholder(std::string_view s) {
  if (!s.starts_with("{") || !s.ends_with("}")) {
    return std::nullopt;
  }
  s = s.substr(1, s.size() - 2);
  bool config;
  if (s.starts_with("config:")) {
    config = true;
    s.remove_prefix(7);
  } else if (s.starts_with("secret:")) {
    config = false;
    s.remove_prefix(7);
  } else {
    return std::nullopt;
  }
  if (!IsKey(s)) {
    return std::nullopt;
  }
  return Placeholder{config, s};
}

bool IsNumber(const base::Value& value) {
  return value.is_int() || value.is_double();
}

bool IsHttpUrl(std::string_view s) {
  GURL url(s);
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS();
}

// A JS number as a base::Value: an int when it is one.
base::Value NumberValue(double d) {
  if (std::trunc(d) == d && std::abs(d) <= 1e9) {
    return base::Value(static_cast<int>(d));
  }
  return base::Value(d);
}

std::string FormatBound(const std::optional<double>& bound) {
  return bound ? base::NumberToString(*bound) : "undefined";
}

}  // namespace

LivePluginManifest::ConfigField::ConfigField() = default;
LivePluginManifest::ConfigField::ConfigField(const ConfigField& other)
    : key(other.key),
      type(other.type),
      label(other.label),
      default_value(other.default_value
                        ? std::optional<base::Value>(other.default_value->Clone())
                        : std::nullopt),
      optional(other.optional),
      placeholder(other.placeholder),
      min(other.min),
      max(other.max),
      options(other.options) {}
LivePluginManifest::ConfigField& LivePluginManifest::ConfigField::operator=(
    const ConfigField& other) {
  if (this != &other) {
    *this = ConfigField(other);
  }
  return *this;
}
LivePluginManifest::ConfigField::ConfigField(ConfigField&&) = default;
LivePluginManifest::ConfigField& LivePluginManifest::ConfigField::operator=(
    ConfigField&&) = default;
LivePluginManifest::ConfigField::~ConfigField() = default;

LivePluginManifest::LivePluginManifest() = default;
LivePluginManifest::LivePluginManifest(const LivePluginManifest&) = default;
LivePluginManifest& LivePluginManifest::operator=(const LivePluginManifest&) =
    default;
LivePluginManifest::LivePluginManifest(LivePluginManifest&&) = default;
LivePluginManifest& LivePluginManifest::operator=(LivePluginManifest&&) =
    default;
LivePluginManifest::~LivePluginManifest() = default;

const LivePluginManifest::ConfigField* LivePluginManifest::FindConfigField(
    std::string_view key) const {
  auto it = std::ranges::find(config, key, &ConfigField::key);
  return it == config.end() ? nullptr : &*it;
}

LiveAllowedHosts::LiveAllowedHosts() = default;
LiveAllowedHosts::LiveAllowedHosts(const LiveAllowedHosts&) = default;
LiveAllowedHosts& LiveAllowedHosts::operator=(const LiveAllowedHosts&) =
    default;
LiveAllowedHosts::~LiveAllowedHosts() = default;

base::expected<LivePluginManifest, LivePluginErrors> ParseLivePluginManifest(
    const base::Value& raw) {
  const base::DictValue* m = raw.GetIfDict();
  if (!m) {
    return base::unexpected(LivePluginErrors{"manifest must be an object"});
  }
  LivePluginErrors errors;
  LivePluginManifest manifest;

  const std::string* id = NonEmptyString(m->Find("id"));
  if (!id || !IsPluginId(*id) || id->size() > kMaxIdLength) {
    errors.push_back(
        "id must be a reverse-DNS style name like \"com.example.feed\"");
  } else {
    manifest.id = *id;
  }
  const std::string* name = NonEmptyString(m->Find("name"));
  if (!name || base::UTF8ToUTF16(*name).size() > kMaxNameLength) {
    errors.push_back("name must be 1-60 characters");
  } else {
    manifest.name = *name;
  }
  const std::string* version = NonEmptyString(m->Find("version"));
  if (!version || !IsVersion(*version)) {
    errors.push_back("version must look like \"1.2.3\"");
  } else {
    manifest.version = *version;
  }
  const base::Value* api = m->Find("api");
  if (!api || !IsNumber(*api) ||
      api->GetDouble() != LivePluginManifest::kApiVersion) {
    errors.push_back("api must be 1");
  }

  const base::Value* entry = m->Find("entry");
  if (!entry) {
    manifest.entry = "main.js";
  } else if (const std::string* e = NonEmptyString(entry);
             e && IsFileName(*e, {"js"}) && !e->starts_with(".")) {
    manifest.entry = *e;
  } else {
    errors.push_back("entry must be a .js file name in the plugin folder");
  }
  if (const base::Value* icon = m->Find("icon")) {
    const std::string* i = NonEmptyString(icon);
    if (!i || !IsFileName(*i, {"svg", "png"})) {
      errors.push_back(
          "icon must be an .svg or .png file name in the plugin folder");
    } else {
      manifest.icon = *i;
    }
  }
  if (const base::Value* description = m->Find("description")) {
    const std::string* d = description->GetIfString();
    if (!d || base::UTF8ToUTF16(*d).size() > kMaxDescriptionLength) {
      errors.push_back("description must be at most 300 characters");
    } else {
      manifest.description = *d;
    }
  }

  // Config fields.
  std::set<std::string, std::less<>> keys;
  const base::Value* config = m->Find("config");
  if (config && !config->is_list()) {
    errors.push_back("config must be a list of fields");
  } else if (config) {
    size_t i = 0;
    for (const base::Value& value : config->GetList()) {
      const std::string where = "config[" + base::NumberToString(i++) + "]";
      const base::DictValue* f = value.GetIfDict();
      if (!f) {
        errors.push_back(where + " must be an object");
        continue;
      }
      ConfigField field;
      const std::string* key = NonEmptyString(f->Find("key"));
      if (!key || !IsKey(*key)) {
        errors.push_back(where + ".key is invalid");
      } else if (base::Contains(keys, *key)) {
        errors.push_back(where + ".key \"" + *key + "\" is repeated");
      } else {
        keys.insert(*key);
        field.key = *key;
      }
      const std::string* type = f->FindString("type");
      static constexpr std::pair<std::string_view, ConfigField::Type>
          kTypes[] = {{"string", ConfigField::Type::kString},
                      {"url", ConfigField::Type::kUrl},
                      {"number", ConfigField::Type::kNumber},
                      {"bool", ConfigField::Type::kBool},
                      {"select", ConfigField::Type::kSelect}};
      auto type_it =
          type ? std::ranges::find(kTypes, std::string_view(*type),
                                   &std::pair<std::string_view,
                                              ConfigField::Type>::first)
               : std::end(kTypes);
      if (type_it == std::end(kTypes)) {
        errors.push_back(where +
                         ".type must be one of string, url, number, bool, "
                         "select");
      } else {
        field.type = type_it->second;
      }
      const std::string* label = NonEmptyString(f->Find("label"));
      if (!label) {
        errors.push_back(where + ".label is required");
      } else {
        field.label = *label;
      }
      if (field.type == ConfigField::Type::kSelect &&
          type_it != std::end(kTypes)) {
        const base::ListValue* options = f->FindList("options");
        if (!options || options->empty()) {
          errors.push_back(where + ".options is required for a select");
        } else {
          bool valid = true;
          for (const base::Value& option : *options) {
            if (const std::string* s = NonEmptyString(&option)) {
              field.options.push_back({*s, *s});
            } else if (const base::DictValue* o = option.GetIfDict();
                       o && NonEmptyString(o->Find("value")) &&
                       NonEmptyString(o->Find("label"))) {
              field.options.push_back(
                  {*o->FindString("value"), *o->FindString("label")});
            } else {
              valid = false;
            }
          }
          if (!valid) {
            errors.push_back(where +
                             ".options must be strings or {value, label}");
          }
        }
      }
      if (const base::Value* def = f->Find("default")) {
        field.default_value = def->Clone();
      }
      field.optional = f->FindBool("optional").value_or(false);
      if (const std::string* placeholder = f->FindString("placeholder")) {
        field.placeholder = *placeholder;
      }
      if (const base::Value* min = f->Find("min"); min && IsNumber(*min)) {
        field.min = min->GetDouble();
      }
      if (const base::Value* max = f->Find("max"); max && IsNumber(*max)) {
        field.max = max->GetDouble();
      }
      manifest.config.push_back(std::move(field));
    }
  }

  // Secrets.
  std::set<std::string, std::less<>> secret_keys;
  const base::Value* secrets = m->Find("secrets");
  if (secrets && !secrets->is_list()) {
    errors.push_back("secrets must be a list");
  } else if (secrets) {
    size_t i = 0;
    for (const base::Value& value : secrets->GetList()) {
      const std::string where = "secrets[" + base::NumberToString(i++) + "]";
      const base::DictValue* s = value.GetIfDict();
      const std::string* key = s ? NonEmptyString(s->Find("key")) : nullptr;
      const std::string* label = s ? NonEmptyString(s->Find("label")) : nullptr;
      if (!key || !IsKey(*key) || !label) {
        errors.push_back(where + " needs a key and a label");
      } else if (base::Contains(secret_keys, *key)) {
        errors.push_back(where + ".key \"" + *key + "\" is repeated");
      } else {
        secret_keys.insert(*key);
        const std::string* help = s->FindString("help");
        manifest.secrets.push_back({*key, *label, help ? *help : std::string(),
                                    s->FindBool("optional").value_or(false)});
      }
    }
  }

  // Hosts and cookie hosts.
  auto check_hosts = [&](const char* what) {
    std::vector<std::string> list;
    const base::Value* value = m->Find(what);
    if (!value) {
      return list;
    }
    if (!value->is_list()) {
      errors.push_back(std::string(what) + " must be a list");
      return list;
    }
    for (const base::Value& entry_value : value->GetList()) {
      const std::string* h = entry_value.GetIfString();
      if (!h) {
        errors.push_back(std::string(what) +
                         ": a host is not a host name, \"*.domain\" or a "
                         "{config:key} placeholder");
        continue;
      }
      if (std::optional<Placeholder> p = ParsePlaceholder(*h)) {
        const bool known = p->config ? base::Contains(keys, p->key)
                                     : base::Contains(secret_keys, p->key);
        const ConfigField* field =
            p->config ? manifest.FindConfigField(p->key) : nullptr;
        if (!known) {
          errors.push_back(std::string(what) + ": " + *h + " names an unknown " +
                           (p->config ? "config" : "secret") + " key");
        } else if (field && field->type != ConfigField::Type::kUrl) {
          errors.push_back(std::string(what) + ": " + *h +
                           " must name a url field");
        }
      } else if (!IsHostPattern(*h)) {
        errors.push_back(std::string(what) + ": \"" + *h +
                         "\" is not a host name, \"*.domain\" or a "
                         "{config:key} placeholder");
      }
      list.push_back(*h);
    }
    return list;
  };
  manifest.hosts = check_hosts("hosts");
  manifest.cookies = check_hosts("cookies");
  for (const std::string& c : manifest.cookies) {
    if (!base::Contains(manifest.hosts, c)) {
      errors.push_back("cookies: \"" + c + "\" must also be listed in hosts");
    }
  }

  // Refresh.
  if (const base::Value* refresh = m->Find("refresh")) {
    const base::DictValue* r = refresh->GetIfDict();
    if (!r) {
      errors.push_back("refresh must be an object");
    } else {
      const double floor = LivePluginManifest::kRefreshFloor.InSecondsF();
      const base::Value* min_value = r->Find("min");
      const base::Value* default_value = r->Find("default");
      const bool min_ok = !min_value || IsNumber(*min_value);
      const double min = min_value && min_ok ? min_value->GetDouble() : floor;
      const bool default_ok = !default_value || IsNumber(*default_value);
      const double def =
          default_value && default_ok
              ? default_value->GetDouble()
              : std::max(min,
                         LivePluginManifest::kDefaultRefresh.InSecondsF());
      if (!min_ok || !default_ok || def < min) {
        errors.push_back(
            "refresh.default and refresh.min must be numbers, default >= min");
      } else {
        manifest.refresh_default = base::Seconds(std::max(floor, def));
        manifest.refresh_min = base::Seconds(std::max(floor, min));
      }
    }
  }

  if (!errors.empty()) {
    return base::unexpected(std::move(errors));
  }
  return manifest;
}

base::expected<base::DictValue, LivePluginErrors> ValidateLiveConfig(
    const LivePluginManifest& manifest,
    const base::DictValue& config) {
  LivePluginErrors errors;
  base::DictValue out;
  for (const ConfigField& f : manifest.config) {
    auto missing = [](const base::Value* v) {
      return !v || v->is_none() ||
             (v->is_string() && v->GetString().empty());
    };
    const base::Value* v = config.Find(f.key);
    if (missing(v)) {
      v = f.default_value ? &*f.default_value : nullptr;
    }
    if (missing(v)) {
      if (!f.optional && f.type != ConfigField::Type::kBool) {
        errors.push_back(f.label + " is required");
      }
      if (f.type == ConfigField::Type::kBool) {
        out.Set(f.key, false);
      }
      continue;
    }
    base::Value value = v->Clone();
    switch (f.type) {
      case ConfigField::Type::kString:
        if (!value.is_string()) {
          errors.push_back(f.label + " must be text");
        }
        break;
      case ConfigField::Type::kUrl:
        if (!value.is_string() || !IsHttpUrl(value.GetString())) {
          errors.push_back(f.label + " must be an http(s) URL");
        }
        break;
      case ConfigField::Type::kNumber: {
        if (value.is_string()) {
          std::string_view s =
              base::TrimWhitespaceASCII(value.GetString(), base::TRIM_ALL);
          double d;
          value = !s.empty() && base::StringToDouble(s, &d) ? NumberValue(d)
                                                            : base::Value();
        }
        if (!IsNumber(value) || !std::isfinite(value.GetDouble())) {
          errors.push_back(f.label + " must be a number");
        } else if ((f.min && value.GetDouble() < *f.min) ||
                   (f.max && value.GetDouble() > *f.max)) {
          errors.push_back(f.label + " must be between " + FormatBound(f.min) +
                           " and " + FormatBound(f.max));
        }
        break;
      }
      case ConfigField::Type::kBool:
        if (value.is_string() && value.GetString() == "true") {
          value = base::Value(true);
        } else if (value.is_string() && value.GetString() == "false") {
          value = base::Value(false);
        }
        if (!value.is_bool()) {
          errors.push_back(f.label + " must be true or false");
        }
        break;
      case ConfigField::Type::kSelect: {
        const std::string* s = value.GetIfString();
        if (!s || std::ranges::find(f.options, *s,
                                    &LivePluginManifest::SelectOption::value) ==
                      f.options.end()) {
          std::vector<std::string_view> values;
          for (const auto& option : f.options) {
            values.push_back(option.value);
          }
          errors.push_back(f.label + " must be one of " +
                           base::JoinString(values, ", "));
        }
        break;
      }
    }
    out.Set(f.key, std::move(value));
  }
  if (!errors.empty()) {
    return base::unexpected(std::move(errors));
  }
  return out;
}

LiveAllowedHosts ResolveLiveHosts(const LivePluginManifest& manifest,
                                  const base::DictValue& config,
                                  const LiveSecrets& secrets) {
  auto resolve = [&](const std::vector<std::string>& list) {
    std::vector<std::string> out;
    for (const std::string& h : list) {
      std::optional<Placeholder> p = ParsePlaceholder(h);
      if (!p) {
        out.push_back(h);
        continue;
      }
      std::string value;
      if (p->config) {
        const std::string* s = config.FindString(p->key);
        if (!s) {
          continue;
        }
        value = *s;
      } else {
        auto it = secrets.find(p->key);
        if (it == secrets.end()) {
          continue;
        }
        value = it->second;
      }
      if (base::StartsWith(value, "webcal:",
                           base::CompareCase::INSENSITIVE_ASCII)) {
        value = "https:" + value.substr(7);
      }
      GURL url(value);
      if (url.is_valid() && url.has_host()) {
        out.push_back(base::ToLowerASCII(url.host()));
      }
    }
    return out;
  };
  LiveAllowedHosts allowed;
  allowed.hosts = resolve(manifest.hosts);
  allowed.cookies = resolve(manifest.cookies);
  return allowed;
}

bool LiveHostMatches(std::string_view pattern, std::string_view host) {
  std::string h = base::ToLowerASCII(host);
  if (h.ends_with('.')) {
    h.pop_back();
  }
  if (pattern.starts_with("*.")) {
    std::string_view domain = pattern.substr(2);
    return h == domain ||
           (h.size() > domain.size() && h.ends_with(domain) &&
            h[h.size() - domain.size() - 1] == '.');
  }
  return h == pattern;
}

bool LiveUrlAllowed(const GURL& url, const std::vector<std::string>& patterns) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || url.has_username() ||
      url.has_password()) {
    return false;
  }
  return std::ranges::any_of(patterns, [&url](const std::string& pattern) {
    return LiveHostMatches(pattern, url.host());
  });
}

}  // namespace compass
