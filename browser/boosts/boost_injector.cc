// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/boost_injector.h"

#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/hash/hash.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/extensions/component_loader.h"
#include "chrome/browser/extensions/component_loader_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/boosts/boosts_assets.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extension_system_provider.h"
#include "extensions/browser/extension_user_script_loader.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/user_script_manager.h"
#include "extensions/common/extension.h"
#include "extensions/common/mojom/execution_world.mojom-shared.h"
#include "extensions/common/mojom/host_id.mojom.h"
#include "extensions/common/mojom/run_location.mojom-shared.h"
#include "extensions/common/url_pattern.h"
#include "extensions/common/user_script.h"

namespace compass::boosts {

namespace {

// The extension's root: the scripts' files, nothing else.
constexpr char kRootName[] = "Compass Boosts Files";

using Files = std::vector<std::pair<std::string, std::string>>;

bool CreateRoot(const base::FilePath& root) {
  return base::CreateDirectory(root);
}

// Writes |files| (name, content) into |root|.
bool WriteFiles(const base::FilePath& root, const Files& files) {
  bool ok = true;
  for (const auto& [name, content] : files) {
    ok &= base::WriteFile(root.AppendASCII(name), content);
  }
  return ok;
}

// Deletes the files of |root| not in |keep|: those of Boosts deleted or
// changed since.
void DeleteOthers(const base::FilePath& root, const std::set<std::string>& keep) {
  base::FileEnumerator files(root, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  for (base::FilePath path = files.Next(); !path.empty();
       path = files.Next()) {
    if (!keep.contains(path.BaseName().AsUTF8Unsafe())) {
      base::DeleteFile(path);
    }
  }
}

}  // namespace

BoostInjector::Script::Script() = default;
BoostInjector::Script::Script(const Script&) = default;
BoostInjector::Script& BoostInjector::Script::operator=(const Script&) =
    default;
BoostInjector::Script::~Script() = default;

BoostInjector::BoostInjector(Profile* profile, BoostStore* store)
    : profile_(profile),
      store_(store),
      root_(profile->GetPath().AppendASCII(kRootName)),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {
  observation_.Observe(store_.get());
  extensions::ExtensionSystem::Get(profile_)->ready().Post(
      FROM_HERE, base::BindOnce(&BoostInjector::OnExtensionSystemReady,
                                weak_factory_.GetWeakPtr()));
}

BoostInjector::~BoostInjector() = default;

// static
BoostInjector* BoostInjector::Get(Profile* profile) {
  return BoostInjectorFactory::GetForProfile(profile);
}

void BoostInjector::OnExtensionSystemReady() {
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&CreateRoot, root_),
      base::BindOnce(&BoostInjector::OnRootReady, weak_factory_.GetWeakPtr()));
}

void BoostInjector::OnRootReady(bool ok) {
  if (!ok) {
    LOG(ERROR) << "[compass-boosts] can't create " << root_;
    return;
  }
  const extensions::ExtensionId id =
      extensions::ComponentLoader::Get(profile_)->Add(assets::kManifest,
                                                      root_);
  if (id != kBoostsExtensionId) {
    LOG(ERROR) << "[compass-boosts] the extension didn't load (" << id << ")";
    return;
  }
  store_->WhenLoaded(base::BindOnce(
      [](base::WeakPtr<BoostInjector> self) {
        if (self) {
          self->ready_ = true;
          self->Update();
        }
      },
      weak_factory_.GetWeakPtr()));
}

void BoostInjector::WhenRegistered(base::OnceClosure done) {
  if (ready_ && !updating_ && !again_) {
    std::move(done).Run();
    return;
  }
  when_registered_.push_back(std::move(done));
}

std::set<std::string> BoostInjector::registered_ids() const {
  std::set<std::string> ids;
  for (const auto& [id, script] : registered_) {
    ids.insert(id);
  }
  return ids;
}

void BoostInjector::OnBoostsChanged() {
  Update();
}

void BoostInjector::Shutdown() {
  observation_.Reset();
  weak_factory_.InvalidateWeakPtrs();
}

std::map<std::string, BoostInjector::Script> BoostInjector::Wanted() const {
  std::map<std::string, Script> wanted;
  for (const Boost& boost : store_->boosts()) {
    if (!boost.enabled) {
      continue;
    }
    base::expected<BoostMatch, std::string> match =
        BoostMatch::Parse(boost.match);
    if (!match.has_value()) {
      continue;
    }
    const std::vector<std::string> patterns = match->ToUrlPatterns();
    const auto add = [&](bool css, const std::string& content) {
      if (base::TrimWhitespaceASCII(content, base::TRIM_ALL).empty()) {
        return;
      }
      Script script;
      script.css = css;
      script.main_world = !css && boost.js_world == kMainWorld;
      script.content = content;
      script.patterns = patterns;
      const uint32_t hash = base::PersistentHash(base::StrCat(
          {content, "\n", script.main_world ? "main" : "isolated", "\n",
           base::JoinString(patterns, " ")}));
      const std::string tag = base::StrCat(
          {boost.id, css ? "-css-" : "-js-", base::NumberToString(hash)});
      script.id =
          base::StrCat({extensions::UserScript::kDynamicContentScriptPrefix,
                        "compass-boost-", tag});
      script.file = base::StrCat({tag, css ? ".css" : ".js"});
      wanted.emplace(script.id, std::move(script));
    };
    add(/*css=*/true, boost.css);
    add(/*css=*/false, boost.js);
  }
  return wanted;
}

void BoostInjector::Update() {
  if (!ready_) {
    return;
  }
  if (updating_) {
    again_ = true;
    return;
  }
  updating_ = true;
  again_ = false;
  std::map<std::string, Script> wanted = Wanted();
  std::set<std::string> to_remove;
  for (const auto& [id, script] : registered_) {
    if (!wanted.contains(id)) {
      to_remove.insert(id);
    }
  }
  std::vector<std::string> to_add;
  Files files;
  for (const auto& [id, script] : wanted) {
    if (!registered_.contains(id)) {
      to_add.push_back(id);
      files.emplace_back(script.file, script.content);
    }
  }
  if (to_remove.empty() && to_add.empty()) {
    FinishPass();
    return;
  }
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&WriteFiles, root_, std::move(files)),
      base::BindOnce(
          [](base::WeakPtr<BoostInjector> self,
             std::map<std::string, Script> wanted,
             std::set<std::string> to_remove, std::vector<std::string> to_add,
             bool ok) {
            if (!self) {
              return;
            }
            if (!ok) {
              LOG(ERROR) << "[compass-boosts] can't write the scripts";
            }
            self->OnFilesWritten(std::move(wanted), std::move(to_remove),
                                 std::move(to_add));
          },
          weak_factory_.GetWeakPtr(), std::move(wanted), std::move(to_remove),
          std::move(to_add)));
}

void BoostInjector::OnFilesWritten(std::map<std::string, Script> wanted,
                                   std::set<std::string> to_remove,
                                   std::vector<std::string> to_add) {
  const extensions::Extension* extension =
      extensions::ExtensionRegistry::Get(profile_)
          ->enabled_extensions()
          .GetByID(kBoostsExtensionId);
  if (!extension) {
    LOG(ERROR) << "[compass-boosts] the extension isn't loaded";
    FinishPass();
    return;
  }
  for (const std::string& id : to_remove) {
    registered_.erase(id);
  }
  extensions::ExtensionSystem::Get(profile_)
      ->user_script_manager()
      ->GetUserScriptLoaderForExtension(kBoostsExtensionId)
      ->RemoveDynamicScripts(
          to_remove,
          base::BindOnce(&BoostInjector::OnRemoved, weak_factory_.GetWeakPtr(),
                         std::move(wanted), std::move(to_add)));
}

void BoostInjector::OnRemoved(std::map<std::string, Script> wanted,
                              std::vector<std::string> to_add,
                              const std::optional<std::string>& error) {
  if (error) {
    LOG(ERROR) << "[compass-boosts] removing scripts: " << *error;
  }
  const extensions::Extension* extension =
      extensions::ExtensionRegistry::Get(profile_)
          ->enabled_extensions()
          .GetByID(kBoostsExtensionId);
  if (!extension || to_add.empty()) {
    FinishPass();
    return;
  }
  extensions::UserScriptList scripts;
  std::set<std::string> ids;
  for (const std::string& id : to_add) {
    const Script& wanted_script = wanted.at(id);
    auto script = std::make_unique<extensions::UserScript>();
    script->set_host_id(extensions::mojom::HostID(
        extensions::mojom::HostID::HostType::kExtensions, extension->id()));
    script->set_id(id);
    // Every frame, as the page itself sees it; never in Incognito, which
    // has no Boosts.
    script->set_match_all_frames(true);
    script->set_incognito_enabled(false);
    for (const std::string& pattern_text : wanted_script.patterns) {
      URLPattern pattern(URLPattern::SCHEME_HTTP | URLPattern::SCHEME_HTTPS);
      if (pattern.Parse(pattern_text) == URLPattern::ParseResult::kSuccess) {
        script->add_url_pattern(pattern);
      }
    }
    std::unique_ptr<extensions::UserScript::Content> file =
        extensions::UserScript::Content::CreateFile(
            extension->path(), base::FilePath::FromASCII(wanted_script.file),
            extension->GetResourceURL(wanted_script.file));
    if (wanted_script.css) {
      // Before the page draws: no flash of the unstyled page.
      script->css_scripts().push_back(std::move(file));
      script->set_run_location(extensions::mojom::RunLocation::kDocumentStart);
    } else {
      script->js_scripts().push_back(std::move(file));
      script->set_run_location(extensions::mojom::RunLocation::kDocumentIdle);
      script->set_execution_world(
          wanted_script.main_world ? extensions::mojom::ExecutionWorld::kMain
                                   : extensions::mojom::ExecutionWorld::kIsolated);
    }
    ids.insert(id);
    scripts.push_back(std::move(script));
  }
  extensions::ExtensionUserScriptLoader* loader =
      extensions::ExtensionSystem::Get(profile_)
          ->user_script_manager()
          ->GetUserScriptLoaderForExtension(kBoostsExtensionId);
  loader->AddPendingDynamicScriptIDs(std::move(ids));
  // Not persisted: the store is the source, registered again at each start.
  loader->AddDynamicScripts(
      std::move(scripts), /*persistent_script_ids=*/{},
      base::BindOnce(&BoostInjector::OnAdded, weak_factory_.GetWeakPtr(),
                     std::move(wanted), std::move(to_add)));
}

void BoostInjector::OnAdded(std::map<std::string, Script> wanted,
                            std::vector<std::string> to_add,
                            const std::optional<std::string>& error) {
  if (error) {
    LOG(ERROR) << "[compass-boosts] adding scripts: " << *error;
  } else {
    for (const std::string& id : to_add) {
      registered_[id] = std::move(wanted.at(id));
    }
  }
  FinishPass();
}

void BoostInjector::FinishPass() {
  updating_ = false;
  if (again_) {
    Update();
    return;
  }
  std::set<std::string> keep;
  for (const auto& [id, script] : registered_) {
    keep.insert(script.file);
  }
  file_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&DeleteOthers, root_, std::move(keep)));
  std::vector<base::OnceClosure> callbacks = std::move(when_registered_);
  for (base::OnceClosure& callback : callbacks) {
    std::move(callback).Run();
  }
}

// static
BoostInjector* BoostInjectorFactory::GetForProfile(Profile* profile) {
  return static_cast<BoostInjector*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
BoostInjectorFactory* BoostInjectorFactory::GetInstance() {
  static base::NoDestructor<BoostInjectorFactory> instance;
  return instance.get();
}

BoostInjectorFactory::BoostInjectorFactory()
    : ProfileKeyedServiceFactory(
          "CompassBoostInjector",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {
  DependsOn(BoostStoreFactory::GetInstance());
  DependsOn(
      extensions::ExtensionsBrowserClient::Get()->GetExtensionSystemFactory());
  DependsOn(extensions::ComponentLoaderFactory::GetInstance());
}

BoostInjectorFactory::~BoostInjectorFactory() = default;

std::unique_ptr<KeyedService>
BoostInjectorFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  BoostStore* store = BoostStore::Get(profile);
  if (!store) {
    return nullptr;
  }
  return std::make_unique<BoostInjector>(profile, store);
}

bool BoostInjectorFactory::ServiceIsCreatedWithBrowserContext() const {
  // The Boosts apply from the first page on.
  return true;
}

}  // namespace compass::boosts
