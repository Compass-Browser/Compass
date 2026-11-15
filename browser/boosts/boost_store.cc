// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/boost_store.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/values_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/types/expected_macros.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "url/gurl.h"
#include "url/url_constants.h"
#include "url/url_util.h"

namespace compass::boosts {

namespace {

constexpr char kFileName[] = "Compass Boosts.json";
constexpr base::TimeDelta kCommitInterval = base::Seconds(1);
constexpr char kWww[] = "www.";

std::optional<std::string> ReadFile(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  return contents;
}

// What sync and the file may bring: a key and a place to apply. Names,
// worlds and sizes from another client are kept as they are.
bool IsValidStored(const Boost& boost) {
  return base::Uuid::ParseLowercase(boost.id).is_valid() &&
         BoostMatch::Parse(boost.match).has_value();
}

void SortByCreated(std::vector<Boost>& boosts) {
  std::ranges::stable_sort(boosts, [](const Boost& a, const Boost& b) {
    return a.created < b.created;
  });
}

std::string StringOr(const base::DictValue& dict,
                     std::string_view key,
                     std::string_view fallback) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string(fallback);
}

bool IsIpAddress(std::string_view host) {
  return url::HostIsIPAddress(host);
}

}  // namespace

Boost::Boost() = default;
Boost::Boost(const Boost&) = default;
Boost& Boost::operator=(const Boost&) = default;
Boost::Boost(Boost&&) = default;
Boost& Boost::operator=(Boost&&) = default;
Boost::~Boost() = default;

base::DictValue Boost::ToDict() const {
  return base::DictValue()
      .Set("id", id)
      .Set("name", name)
      .Set("match", match)
      .Set("css", css)
      .Set("js", js)
      .Set("js_world", js_world)
      .Set("enabled", enabled)
      .Set("author", author)
      .Set("create_time", base::TimeToValue(created))
      .Set("update_time", base::TimeToValue(updated));
}

// static
std::optional<Boost> Boost::FromDict(const base::DictValue& dict) {
  const std::string* id_value = dict.FindString("id");
  const std::string* match_value = dict.FindString("match");
  if (!id_value || !match_value) {
    return std::nullopt;
  }
  Boost boost;
  boost.id = *id_value;
  boost.match = *match_value;
  boost.name = StringOr(dict, "name", *match_value);
  boost.css = StringOr(dict, "css", "");
  boost.js = StringOr(dict, "js", "");
  boost.js_world = StringOr(dict, "js_world", kIsolatedWorld);
  boost.enabled = dict.FindBool("enabled").value_or(true);
  boost.author = StringOr(dict, "author", kAuthorUser);
  boost.created =
      base::ValueToTime(dict.Find("create_time")).value_or(base::Time());
  boost.updated =
      base::ValueToTime(dict.Find("update_time")).value_or(boost.created);
  return boost;
}

// static
base::expected<BoostMatch, std::string> BoostMatch::Parse(
    std::string_view match) {
  std::string_view rest = base::TrimWhitespaceASCII(match, base::TRIM_ALL);
  const auto invalid = [&match]() {
    return base::unexpected(base::StrCat(
        {"\"", match,
         "\" isn't a site pattern. Use a host (\"example.com\", which also "
         "covers www.example.com), a domain with its subdomains "
         "(\"*.example.com\"), and optionally a path prefix "
         "(\"github.com/settings\")."}));
  };
  if (rest.empty()) {
    return invalid();
  }
  // A scheme typed or pasted: only the web's.
  if (const size_t scheme_end = rest.find("://");
      scheme_end != std::string_view::npos) {
    const std::string scheme =
        base::ToLowerASCII(rest.substr(0, scheme_end));
    if (scheme != url::kHttpScheme && scheme != url::kHttpsScheme &&
        scheme != "*") {
      return base::unexpected(
          "Boosts apply to web pages only (http and https).");
    }
    rest.remove_prefix(scheme_end + 3);
  }
  const size_t slash = rest.find('/');
  std::string_view host_part = rest.substr(0, slash);
  std::string_view path_part =
      slash == std::string_view::npos ? std::string_view() : rest.substr(slash);

  BoostMatch parsed;
  if (host_part.starts_with("*.")) {
    parsed.subdomains = true;
    host_part.remove_prefix(2);
  }
  if (host_part.empty() || host_part.find_first_of("*:@ ") !=
                               std::string_view::npos) {
    return invalid();
  }
  const GURL canonical(base::StrCat({"https://", host_part, "/"}));
  if (!canonical.is_valid() || canonical.host().empty()) {
    return invalid();
  }
  std::string name(canonical.host());
  while (name.ends_with('.')) {
    name.pop_back();
  }
  if (name.empty()) {
    return invalid();
  }
  if (!parsed.subdomains && name.starts_with(kWww) &&
      name.size() > strlen(kWww)) {
    // "www.example.com" is example.com, which covers it.
    name = name.substr(strlen(kWww));
  }
  if (parsed.subdomains &&
      (name.find('.') == std::string::npos || IsIpAddress(name))) {
    // "*.com" would be most of the web.
    return base::unexpected(base::StrCat(
        {"\"", match, "\" covers too much: name a site's domain, like "
                      "\"*.example.com\"."}));
  }
  parsed.host = std::move(name);

  std::string prefix(path_part);
  while (prefix.ends_with('*') || prefix.ends_with('/')) {
    prefix.pop_back();
  }
  if (prefix.find_first_of("*?# ") != std::string::npos) {
    return invalid();
  }
  parsed.path = std::move(prefix);
  return parsed;
}

std::string BoostMatch::ToString() const {
  return base::StrCat({subdomains ? "*." : "", host, path});
}

bool BoostMatch::MatchesHost(std::string_view page_host) const {
  std::string_view page = page_host;
  while (page.ends_with('.')) {
    page.remove_suffix(1);
  }
  if (page == host) {
    return true;
  }
  if (subdomains) {
    return page.ends_with(base::StrCat({".", host}));
  }
  return page.starts_with(kWww) && page.substr(strlen(kWww)) == host;
}

bool BoostMatch::Matches(const GURL& url) const {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  return MatchesHost(url.host()) && url.path().starts_with(path);
}

std::vector<std::string> BoostMatch::ToUrlPatterns() const {
  const std::string path_glob = base::StrCat({path.empty() ? "/" : path, "*"});
  if (subdomains) {
    return {base::StrCat({"*://*.", host, path_glob})};
  }
  std::vector<std::string> patterns = {
      base::StrCat({"*://", host, path_glob})};
  if (!IsIpAddress(host)) {
    patterns.push_back(base::StrCat({"*://", kWww, host, path_glob}));
  }
  return patterns;
}

base::expected<Boost, std::string> NormalizeBoost(Boost boost) {
  if (!boost.id.empty() && !base::Uuid::ParseLowercase(boost.id).is_valid()) {
    return base::unexpected(
        base::StrCat({"No Boost has the id ", boost.id, "."}));
  }
  ASSIGN_OR_RETURN(BoostMatch match, BoostMatch::Parse(boost.match));
  boost.match = match.ToString();

  std::u16string name = base::CollapseWhitespace(
      base::UTF8ToUTF16(boost.name), /*trim_sequences_with_line_breaks=*/false);
  if (name.empty()) {
    name = base::UTF8ToUTF16(boost.match);
  }
  if (name.size() > kMaxBoostNameChars) {
    return base::unexpected(base::StrCat(
        {"A Boost's name is at most ", base::NumberToString(kMaxBoostNameChars),
         " characters."}));
  }
  boost.name = base::UTF16ToUTF8(name);

  if (boost.js_world.empty()) {
    boost.js_world = kIsolatedWorld;
  }
  if (boost.js_world != kIsolatedWorld && boost.js_world != kMainWorld) {
    return base::unexpected(
        "js_world is \"isolated\" (the default) or \"main\".");
  }
  if (boost.author != kAuthorClaude) {
    boost.author = kAuthorUser;
  }
  if (base::TrimWhitespaceASCII(boost.css, base::TRIM_ALL).empty() &&
      base::TrimWhitespaceASCII(boost.js, base::TRIM_ALL).empty()) {
    return base::unexpected("A Boost needs CSS, JavaScript or both.");
  }
  if (boost.css.size() + boost.js.size() > kMaxBoostBytes) {
    return base::unexpected(base::StrCat(
        {"A Boost's CSS and JavaScript are at most ",
         base::NumberToString(kMaxBoostBytes / 1024),
         " KB together. Keep it small and specific to the site."}));
  }
  return base::ok(std::move(boost));
}

BoostStore::BoostStore(const base::FilePath& path)
    : file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      writer_(path, file_task_runner_, kCommitInterval, "CompassBoosts") {
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadFile, writer_.path()),
      base::BindOnce(&BoostStore::OnRead, weak_factory_.GetWeakPtr()));
}

BoostStore::~BoostStore() {
  Flush();
}

// static
BoostStore* BoostStore::Get(Profile* profile) {
  return BoostStoreFactory::GetForProfile(profile);
}

void BoostStore::WhenLoaded(base::OnceClosure done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (loaded_) {
    std::move(done).Run();
    return;
  }
  when_loaded_.push_back(std::move(done));
}

void BoostStore::OnRead(std::optional<std::string> contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::optional<base::Value> value =
      contents ? base::JSONReader::Read(*contents, base::JSON_PARSE_RFC)
               : std::nullopt;
  std::vector<Boost> read;
  if (value && value->is_list()) {
    for (const base::Value& item : value->GetList()) {
      if (!item.is_dict()) {
        continue;
      }
      std::optional<Boost> boost = Boost::FromDict(item.GetDict());
      if (boost && IsValidStored(*boost) &&
          !std::ranges::contains(read, boost->id, &Boost::id)) {
        read.push_back(std::move(*boost));
      }
    }
  }
  boosts_ = std::move(read);
  SortByCreated(boosts_);
  loaded_ = true;
  std::vector<base::OnceClosure> callbacks = std::move(when_loaded_);
  for (base::OnceClosure& callback : callbacks) {
    std::move(callback).Run();
  }
  for (Observer& observer : observers_) {
    observer.OnBoostsChanged();
  }
}

const Boost* BoostStore::Find(std::string_view id) const {
  const std::string lower = base::ToLowerASCII(id);
  auto it = std::ranges::find(boosts_, lower, &Boost::id);
  return it == boosts_.end() ? nullptr : &*it;
}

std::vector<const Boost*> BoostStore::ForUrl(const GURL& url) const {
  std::vector<const Boost*> found;
  for (const Boost& boost : boosts_) {
    base::expected<BoostMatch, std::string> match =
        BoostMatch::Parse(boost.match);
    if (match.has_value() && match->Matches(url)) {
      found.push_back(&boost);
    }
  }
  return found;
}

base::expected<Boost, std::string> BoostStore::Save(Boost boost) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_) {
    return base::unexpected("Boosts are still loading. Try again in a moment.");
  }
  boost.id = base::ToLowerASCII(boost.id);
  ASSIGN_OR_RETURN(Boost normalized, NormalizeBoost(std::move(boost)));
  if (normalized.id.empty()) {
    if (boosts_.size() >= kMaxBoosts) {
      return base::unexpected(base::StrCat(
          {"There are already ", base::NumberToString(kMaxBoosts),
           " Boosts. Delete one that's no longer needed first."}));
    }
    normalized.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    normalized.created = normalized.updated = base::Time::Now();
    boosts_.push_back(normalized);
    Changed();
    return normalized;
  }
  auto it = std::ranges::find(boosts_, normalized.id, &Boost::id);
  if (it == boosts_.end()) {
    return base::unexpected(base::StrCat(
        {"No Boost has the id ", normalized.id,
         ". It may have been deleted; save it without an id to make a new "
         "one."}));
  }
  normalized.author = it->author;
  normalized.created = it->created;
  normalized.updated = it->updated;
  if (normalized == *it) {
    return normalized;
  }
  normalized.updated = std::max(base::Time::Now(), it->updated);
  *it = normalized;
  Changed();
  return normalized;
}

bool BoostStore::SetEnabled(std::string_view id, bool enabled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = std::ranges::find(boosts_, base::ToLowerASCII(id), &Boost::id);
  if (it == boosts_.end()) {
    return false;
  }
  if (it->enabled != enabled) {
    it->enabled = enabled;
    it->updated = std::max(base::Time::Now(), it->updated);
    Changed();
  }
  return true;
}

std::optional<Boost> BoostStore::Remove(std::string_view id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = std::ranges::find(boosts_, base::ToLowerASCII(id), &Boost::id);
  if (it == boosts_.end()) {
    return std::nullopt;
  }
  Boost removed = std::move(*it);
  boosts_.erase(it);
  Changed();
  return removed;
}

bool BoostStore::Restore(const Boost& boost) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_ || !IsValidStored(boost)) {
    return false;
  }
  Boost restored = boost;
  // Newer than any copy sync may have: the restored one wins.
  restored.updated = std::max(base::Time::Now(), boost.updated);
  auto it = std::ranges::find(boosts_, boost.id, &Boost::id);
  if (it != boosts_.end()) {
    *it = std::move(restored);
  } else {
    if (boosts_.size() >= kMaxBoosts) {
      return false;
    }
    boosts_.push_back(std::move(restored));
    SortByCreated(boosts_);
  }
  Changed();
  return true;
}

void BoostStore::ApplyRemote(const std::vector<Boost>& upserts,
                             const std::set<std::string>& deleted) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_) {
    // After the file, which it then changes.
    when_loaded_.push_back(base::BindOnce(&BoostStore::ApplyRemote,
                                          weak_factory_.GetWeakPtr(), upserts,
                                          deleted));
    return;
  }
  bool changed = false;
  for (const Boost& remote : upserts) {
    if (!IsValidStored(remote)) {
      continue;
    }
    auto it = std::ranges::find(boosts_, remote.id, &Boost::id);
    if (it == boosts_.end()) {
      // Not capped: one dropped here would be deleted from the account by
      // the next pass.
      boosts_.push_back(remote);
      changed = true;
      continue;
    }
    // The later edit wins; this device's goes up with the next pass.
    if (remote.updated >= it->updated && *it != remote) {
      *it = remote;
      changed = true;
    }
  }
  const size_t before = boosts_.size();
  std::erase_if(boosts_, [&deleted](const Boost& boost) {
    return deleted.contains(boost.id);
  });
  changed |= boosts_.size() != before;
  if (changed) {
    SortByCreated(boosts_);
    Changed();
  }
}

void BoostStore::Changed() {
  if (loaded_) {
    writer_.ScheduleWrite(this);
  }
  for (Observer& observer : observers_) {
    observer.OnBoostsChanged();
  }
}

void BoostStore::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void BoostStore::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void BoostStore::Flush() {
  if (writer_.HasPendingWrite()) {
    writer_.DoScheduledWrite();
  }
}

std::optional<std::string> BoostStore::SerializeData() {
  base::ListValue list;
  for (const Boost& boost : boosts_) {
    list.Append(boost.ToDict());
  }
  return base::WriteJsonWithOptions(list,
                                    base::JSONWriter::OPTIONS_PRETTY_PRINT);
}

void BoostStore::Shutdown() {
  Flush();
}

// static
BoostStore* BoostStoreFactory::GetForProfile(Profile* profile) {
  return static_cast<BoostStore*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
BoostStoreFactory* BoostStoreFactory::GetInstance() {
  static base::NoDestructor<BoostStoreFactory> instance;
  return instance.get();
}

BoostStoreFactory::BoostStoreFactory()
    : ProfileKeyedServiceFactory(
          "CompassBoostStore",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

BoostStoreFactory::~BoostStoreFactory() = default;

std::unique_ptr<KeyedService>
BoostStoreFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<BoostStore>(
      Profile::FromBrowserContext(context)->GetPath().AppendASCII(kFileName));
}

bool BoostStoreFactory::ServiceIsCreatedWithBrowserContext() const {
  // Sync applies remote Boosts, and the injector registers them, at startup.
  return true;
}

}  // namespace compass::boosts
