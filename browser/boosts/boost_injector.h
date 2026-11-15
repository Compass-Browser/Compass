// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BOOSTS_BOOST_INJECTOR_H_
#define COMPASS_BROWSER_BOOSTS_BOOST_INJECTOR_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "compass/browser/boosts/boost_store.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace compass::boosts {

// The component extension the Boosts run as (resources/boosts/
// manifest.json's key). Hidden, as component extensions are.
inline constexpr char kBoostsExtensionId[] = "bmicmjdbmhbbcjkbfnjbphgocpjlfjbk";

// Runs a profile's enabled Boosts on the pages they match, through
// Chromium's content scripts (docs/design/boosts.md, "Injection"): it adds
// the "Compass Boosts" component extension, whose root is
// <profile>/Compass Boosts Files, and registers each Boost's CSS (at
// document start) and JavaScript (at document idle, in the isolated or main
// world) as dynamic content scripts of it, in every frame. The scripts are
// files there, named after their content, since the loader reads a script's
// content from its file. Every change to the store registers the
// difference: pages loaded from then on get it.
class BoostInjector : public KeyedService, public BoostStore::Observer {
 public:
  BoostInjector(Profile* profile, BoostStore* store);
  BoostInjector(const BoostInjector&) = delete;
  BoostInjector& operator=(const BoostInjector&) = delete;
  ~BoostInjector() override;

  static BoostInjector* Get(Profile* profile);

  // |done| runs once what's registered matches the store (at once if it
  // does now, or if the extension couldn't load). For tests, and for
  // Claude's tools, which answer once a save applies.
  void WhenRegistered(base::OnceClosure done);

  // The script ids registered now, for tests.
  std::set<std::string> registered_ids() const;

  // BoostStore::Observer:
  void OnBoostsChanged() override;

  // KeyedService:
  void Shutdown() override;

 private:
  // A content script for one Boost's CSS or JS.
  struct Script {
    Script();
    Script(const Script&);
    Script& operator=(const Script&);
    ~Script();

    // "_dc_" + the Boost's id + kind + a hash of what's below: any change
    // makes another id.
    std::string id;
    // Its file in root_.
    std::string file;
    std::string content;
    bool css = false;
    bool main_world = false;
    std::vector<std::string> patterns;
  };

  void OnExtensionSystemReady();
  // The extension couldn't load: Boosts don't run, and nobody waits.
  void Fail();
  void OnRootReady(bool ok);
  // What the store's enabled Boosts need, by id.
  std::map<std::string, Script> Wanted() const;
  // Registers the difference between Wanted() and registered_, one pass at
  // a time; another change during a pass starts another after it.
  void Update();
  void OnFilesWritten(std::map<std::string, Script> wanted,
                      std::set<std::string> to_remove,
                      std::vector<std::string> to_add);
  void OnRemoved(std::map<std::string, Script> wanted,
                 std::vector<std::string> to_add,
                 const std::optional<std::string>& error);
  void OnAdded(std::map<std::string, Script> wanted,
               std::vector<std::string> to_add,
               const std::optional<std::string>& error);
  void FinishPass();

  const raw_ptr<Profile> profile_;
  const raw_ptr<BoostStore> store_;
  const base::FilePath root_;
  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  // The extension is loaded, and the store too: passes can run.
  bool ready_ = false;
  bool failed_ = false;
  bool updating_ = false;
  bool again_ = false;
  // Registered scripts by id, as they were registered.
  std::map<std::string, Script> registered_;
  std::vector<base::OnceClosure> when_registered_;
  base::ScopedObservation<BoostStore, BoostStore::Observer> observation_{
      this};
  base::WeakPtrFactory<BoostInjector> weak_factory_{this};
};

class BoostInjectorFactory : public ProfileKeyedServiceFactory {
 public:
  static BoostInjectorFactory* GetInstance();
  // Null for profiles without Boosts.
  static BoostInjector* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<BoostInjectorFactory>;

  BoostInjectorFactory();
  ~BoostInjectorFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace compass::boosts

#endif  // COMPASS_BROWSER_BOOSTS_BOOST_INJECTOR_H_
