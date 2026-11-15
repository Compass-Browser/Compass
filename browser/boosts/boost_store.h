// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BOOSTS_BOOST_STORE_H_
#define COMPASS_BROWSER_BOOSTS_BOOST_STORE_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"

class GURL;
class Profile;

namespace compass::boosts {

// "isolated" and "main": where a Boost's JavaScript runs.
inline constexpr char kIsolatedWorld[] = "isolated";
inline constexpr char kMainWorld[] = "main";
// Who made a Boost.
inline constexpr char kAuthorUser[] = "user";
inline constexpr char kAuthorClaude[] = "claude";

// A Boost's CSS and JavaScript together, at most, in bytes.
inline constexpr size_t kMaxBoostBytes = 256 * 1024;
// The most Boosts the user or Claude can make (sync may bring more).
inline constexpr size_t kMaxBoosts = 200;
// A Boost's longest name, in characters.
inline constexpr size_t kMaxBoostNameChars = 100;

// Per-site CSS and JavaScript (docs/design/boosts.md).
struct Boost {
  Boost();
  Boost(const Boost&);
  Boost& operator=(const Boost&);
  Boost(Boost&&);
  Boost& operator=(Boost&&);
  ~Boost();

  // A lowercase UUID; also its sync client tag.
  std::string id;
  std::string name;
  // A host pattern with an optional path prefix (BoostMatch).
  std::string match;
  std::string css;
  std::string js;
  // kIsolatedWorld or kMainWorld; a value from a newer client is kept and
  // runs as isolated.
  std::string js_world = kIsolatedWorld;
  bool enabled = true;
  // kAuthorUser or kAuthorClaude: who made it.
  std::string author = kAuthorUser;
  base::Time created;
  base::Time updated;

  bool operator==(const Boost&) const = default;

  // Its object in Compass Boosts.json and in tool results: snake_case keys,
  // times as microseconds since the Windows epoch in strings.
  base::DictValue ToDict() const;
  // Null if |dict| has no id or match.
  static std::optional<Boost> FromDict(const base::DictValue& dict);
};

// Where a Boost applies, parsed from its `match`:
//   "example.com"          that host and www.example.com
//   "*.example.com"        example.com and all its subdomains
//   "github.com/settings"  either, on paths that start with "/settings"
// http and https only; any port.
struct BoostMatch {
  // Lowercase ASCII (IDN as punycode), without "*.".
  std::string host;
  // "*.": the host and its subdomains.
  bool subdomains = false;
  // Empty, or starting with "/".
  std::string path;

  bool operator==(const BoostMatch&) const = default;

  // Parses |match|, leniently: a scheme, "www." handling and a trailing "*"
  // or "/" are taken off. An error for what isn't a host pattern.
  static base::expected<BoostMatch, std::string> Parse(std::string_view match);

  // The normalized pattern: what Parse() gives back unchanged.
  std::string ToString() const;
  // Whether a page at |url| gets the Boost.
  bool Matches(const GURL& url) const;
  // Whether the Boost applies somewhere on |host| (list_boosts' filter):
  // its path is ignored.
  bool MatchesHost(std::string_view host) const;
  // The extension URL patterns that match the same pages, roughly: a path
  // prefix ends in "*", so "/settings" also matches "/settingsfoo".
  std::vector<std::string> ToUrlPatterns() const;
};

// Checks and normalizes what a user or Claude saves: the match, the name
// (the match when empty), the world, the size. Keeps id, author and times.
base::expected<Boost, std::string> NormalizeBoost(Boost boost);

// A profile's Boosts, kept in <profile>/Compass Boosts.json as a JSON array
// of Boost::ToDict() objects. Not in Incognito or Guest. Syncs with
// COMPASS_SPACES (docs/design/sync.md, "Boosts").
class BoostStore : public KeyedService,
                   public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Any change: local, or from sync.
    virtual void OnBoostsChanged() = 0;
  };

  explicit BoostStore(const base::FilePath& path);
  BoostStore(const BoostStore&) = delete;
  BoostStore& operator=(const BoostStore&) = delete;
  ~BoostStore() override;

  // Null for profiles without Boosts (Incognito, Guest).
  static BoostStore* Get(Profile* profile);

  // |done| runs once the file is read, or at once if it was.
  void WhenLoaded(base::OnceClosure done);
  bool loaded() const { return loaded_; }

  // Oldest first.
  const std::vector<Boost>& boosts() const { return boosts_; }
  const Boost* Find(std::string_view id) const;
  // Those that apply to |url|, enabled or not.
  std::vector<const Boost*> ForUrl(const GURL& url) const;

  // Adds |boost| (no id: a new one, made now) or updates the one with its
  // id, which keeps its author and creation time. The saved Boost, or why
  // not (NormalizeBoost(), full, no such id, still loading).
  base::expected<Boost, std::string> Save(Boost boost);
  // False if there's no such Boost.
  bool SetEnabled(std::string_view id, bool enabled);
  // The removed Boost, if there was one.
  std::optional<Boost> Remove(std::string_view id);
  // Puts |boost| back as it was (Undo). False if it can't (full, invalid).
  bool Restore(const Boost& boost);

  // Sync: |upserts| replace or add Boosts by id, unless the local one was
  // updated later; |deleted| go. Before the file is read, waits for it.
  void ApplyRemote(const std::vector<Boost>& upserts,
                   const std::set<std::string>& deleted);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Writes pending changes now (shutdown, tests).
  void Flush();

  base::WeakPtr<BoostStore> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

  // KeyedService:
  void Shutdown() override;

 private:
  void OnRead(std::optional<std::string> contents);
  void Changed();

  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  base::ImportantFileWriter writer_;
  bool loaded_ = false;
  std::vector<base::OnceClosure> when_loaded_;
  std::vector<Boost> boosts_;
  base::ObserverList<Observer> observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<BoostStore> weak_factory_{this};
};

class BoostStoreFactory : public ProfileKeyedServiceFactory {
 public:
  static BoostStoreFactory* GetInstance();
  // Null for profiles without Boosts.
  static BoostStore* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<BoostStoreFactory>;

  BoostStoreFactory();
  ~BoostStoreFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace compass::boosts

#endif  // COMPASS_BROWSER_BOOSTS_BOOST_STORE_H_
