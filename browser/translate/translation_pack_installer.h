// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_TRANSLATION_PACK_INSTALLER_H_
#define COMPASS_BROWSER_TRANSLATE_TRANSLATION_PACK_INSTALLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "components/on_device_translation/public/language_pack.h"

class PrefRegistrySimple;
class PrefService;

namespace network {
class SharedURLLoaderFactory;
}

// Language packs from Compass's mirror of Firefox's models, for upstream's
// on-device translation
// service (docs/design/translation.md, "Packs on the device"). Upstream's
// installer calls in here instead of the component updater
// (chromium_src/chrome/browser/on_device_translation/installer_impl.cc),
// and reads what's installed from its own prefs, which this sets.

namespace compass::translate {

struct Manifest;

// Local state: the user agreed to model downloads ("Asking first").
// Nothing is requested without it. Registered by
// compass::RegisterCompassLocalStatePrefs() (browser/prefs/compass_prefs.cc).
inline constexpr char kModelDownloadsAllowed[] =
    "compass.translate.model_downloads_allowed";

// For tests' own local state.
void RegisterLocalStatePrefs(PrefRegistrySimple* registry);

class PackInstaller {
 public:
  using Key = on_device_translation::LanguagePackKey;

  static PackInstaller& Get();

  PackInstaller();
  ~PackInstaller();
  PackInstaller(const PackInstaller&) = delete;
  PackInstaller& operator=(const PackInstaller&) = delete;

  // Downloads both directions of `key`'s pair, checks them, installs them in
  // the packs folder and records the path in upstream's prefs on
  // `local_state`, then runs `on_ready`. A second call while one runs
  // waits for it. Does nothing without kModelDownloadsAllowed.
  void Install(Key key,
               PrefService* local_state,
               base::RepeatingClosure on_ready);

  // Runs `callback` with the result of `key`'s install: the one running, or
  // the next one (an install refused for want of consent says false).
  void OnInstalled(Key key, base::OnceCallback<void(bool)> callback);

  // Forgets the pack and deletes its folder.
  void Uninstall(Key key, PrefService* local_state);

  // <user data>/Compass Translation: one folder per pack, as upstream names
  // them ("de_en"), with a folder per direction ("de-en", "en-de").
  base::FilePath packs_dir() const;

  // Tests: where files come from, and each install's result.
  void SetForTesting(scoped_refptr<network::SharedURLLoaderFactory> factory,
                     const Manifest* manifest,
                     base::FilePath packs_dir);
  void SetResultCallbackForTesting(
      base::RepeatingCallback<void(Key, bool)> callback);

 private:
  struct Job;

  void Next(Key key);
  void OnDownloaded(Key key, base::FilePath download);
  void OnWritten(Key key, bool ok);
  void OnMoved(Key key, bool ok);
  void Finish(Key key, bool ok);
  void Report(Key key, bool ok);

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory();
  const Manifest& manifest() const;

  std::map<Key, std::unique_ptr<Job>> jobs_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_for_testing_;
  raw_ptr<const Manifest> manifest_for_testing_ = nullptr;
  base::FilePath packs_dir_for_testing_;
  base::RepeatingCallback<void(Key, bool)> result_callback_;
  std::map<Key, std::vector<base::OnceCallback<void(bool)>>> waiters_;
  base::WeakPtrFactory<PackInstaller> weak_factory_{this};
};

}  // namespace compass::translate

#endif  // COMPASS_BROWSER_TRANSLATE_TRANSLATION_PACK_INSTALLER_H_
