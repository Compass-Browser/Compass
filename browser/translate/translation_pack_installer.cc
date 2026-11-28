// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translation_pack_installer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/chrome_paths.h"
#include "compass/browser/translate/translation_manifest.h"
#include "components/on_device_translation/public/language_pack.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "crypto/hash.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/zstd/src/lib/zstd.h"

namespace compass::translate {

namespace {

using on_device_translation::GetComponentPathPrefName;
using on_device_translation::GetLanguagePackComponentConfig;
using on_device_translation::GetPackageInstallDirName;
using on_device_translation::GetRegisteredFlagPrefName;
using on_device_translation::GetSourceLanguageCode;
using on_device_translation::GetTargetLanguageCode;

constexpr base::FilePath::CharType kPacksFolder[] =
    FILE_PATH_LITERAL("Compass Translation");

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_translation_models", R"(
        semantics {
          sender: "Compass page translation"
          description:
            "Downloads a language's translation model (Firefox's, which "
            "Compass mirrors) so pages translate on the device."
          trigger:
            "The user translates a page, or a website asks for a "
            "translation, in a language whose model isn't downloaded yet, "
            "after the user agreed to downloads from Mozilla."
          data: "None; the request names the model file."
          destination: OTHER
          destination_other: "Compass's mirror, "
            "assets.compassbrowser.org/translation/."
        }
        policy {
          cookies_allowed: NO
          setting:
            "Settings, Languages: Download translation languages."
          policy_exception_justification: "Not implemented."
        })");

// The engine's names for the files (components/translatekit/engine.h).
const char* FileName(const std::string& type) {
  if (type == "model") {
    return "model.bin";
  }
  if (type == "vocab") {
    return "vocab.spm";
  }
  if (type == "srcvocab") {
    return "srcvocab.spm";
  }
  return "trgvocab.spm";
}

std::string Sha256Hex(base::span<const uint8_t> data) {
  return base::ToLowerASCII(base::HexEncode(crypto::hash::Sha256(data)));
}

// Checks the downloaded zstd file, decompresses it, checks the result,
// writes it and deletes the download. Off the UI thread.
bool CheckAndWrite(base::FilePath download,
                   ModelFile file,
                   base::FilePath destination) {
  std::string compressed;
  bool read = base::ReadFileToString(download, &compressed);
  base::DeleteFile(download);
  if (!read || static_cast<int64_t>(compressed.size()) != file.size ||
      Sha256Hex(base::as_byte_span(compressed)) != file.hash) {
    LOG(ERROR) << "Translation model doesn't match: " << file.location;
    return false;
  }
  ZSTD_DCtx* context = ZSTD_createDCtx();
  std::string plain;
  plain.reserve(static_cast<size_t>(file.decompressed_size));
  ZSTD_inBuffer in{compressed.data(), compressed.size(), 0};
  std::vector<char> chunk(ZSTD_DStreamOutSize());
  bool ok = true;
  while (in.pos < in.size) {
    ZSTD_outBuffer out{chunk.data(), chunk.size(), 0};
    size_t result = ZSTD_decompressStream(context, &out, &in);
    if (ZSTD_isError(result)) {
      ok = false;
      break;
    }
    plain.append(chunk.data(), out.pos);
    if (static_cast<int64_t>(plain.size()) > file.decompressed_size) {
      ok = false;
      break;
    }
  }
  ZSTD_freeDCtx(context);
  if (!ok || static_cast<int64_t>(plain.size()) != file.decompressed_size ||
      Sha256Hex(base::as_byte_span(plain)) != file.decompressed_hash) {
    LOG(ERROR) << "Translation model doesn't decompress as expected: "
               << file.location;
    return false;
  }
  return base::CreateDirectory(destination.DirName()) &&
         base::WriteFile(destination, plain);
}

bool PrepareStaging(base::FilePath staging) {
  base::DeletePathRecursively(staging);
  return base::CreateDirectory(staging);
}

bool MoveIntoPlace(base::FilePath staging, base::FilePath destination) {
  base::DeletePathRecursively(destination);
  return base::Move(staging, destination);
}

std::unique_ptr<network::SimpleURLLoader> MakeLoader(const GURL& url) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  return network::SimpleURLLoader::Create(std::move(request),
                                          kTrafficAnnotation);
}

int ResponseCode(const network::SimpleURLLoader& loader) {
  const network::mojom::URLResponseHead* head = loader.ResponseInfo();
  return head && head->headers ? head->headers->response_code() : 0;
}

}  // namespace

void RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kModelDownloadsAllowed, false);
}

struct PackInstaller::Job {
  struct Item {
    std::string direction;  // "de-en", upstream's codes.
    std::string from;       // Mozilla's codes.
    std::string to;
    ModelFile file;
  };

  raw_ptr<PrefService> local_state;
  std::vector<base::RepeatingClosure> on_ready;
  std::vector<Item> items;
  size_t next = 0;
  // Compressed bytes in all, and of the files already downloaded.
  int64_t total = 0;
  int64_t done = 0;
  base::FilePath staging;
  base::FilePath destination;
  std::string attachments;
  std::unique_ptr<network::SimpleURLLoader> loader;
};

// static
PackInstaller& PackInstaller::Get() {
  static base::NoDestructor<PackInstaller> instance;
  return *instance;
}

PackInstaller::PackInstaller() = default;
PackInstaller::~PackInstaller() = default;

base::FilePath PackInstaller::packs_dir() const {
  if (!packs_dir_for_testing_.empty()) {
    return packs_dir_for_testing_;
  }
  base::FilePath user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  return user_data.Append(kPacksFolder);
}

void PackInstaller::Install(Key key,
                            PrefService* local_state,
                            base::RepeatingClosure on_ready) {
  if (auto it = jobs_.find(key); it != jobs_.end()) {
    it->second->on_ready.push_back(std::move(on_ready));
    return;
  }
  if (!local_state->GetBoolean(kModelDownloadsAllowed)) {
    // Page translation asks first (docs/design/translation.md, "Asking
    // first"); a website's Translator API never starts a download.
    Report(key, false);
    return;
  }
  auto job = std::make_unique<Job>();
  job->local_state = local_state;
  job->on_ready.push_back(std::move(on_ready));
  job->attachments = manifest().attachments;
  const std::string language1(GetSourceLanguageCode(key));
  const std::string language2(GetTargetLanguageCode(key));
  for (const auto& [from, to] :
       {std::pair(language1, language2), std::pair(language2, language1)}) {
    const std::string model_from = ToModelLanguage(from);
    const std::string model_to = ToModelLanguage(to);
    const ModelDirection* direction = manifest().Find(model_from, model_to);
    if (!direction) {
      LOG(ERROR) << "No translation model for " << from << "-" << to;
      Report(key, false);
      return;
    }
    for (const ModelFile& file : direction->files) {
      job->items.push_back({from + "-" + to, model_from, model_to, file});
    }
  }
  for (const Job::Item& item : job->items) {
    job->total += item.file.size;
  }
  const std::string folder = GetPackageInstallDirName(key);
  job->destination = packs_dir().AppendASCII(folder);
  job->staging = packs_dir().AppendASCII(".staging-" + folder);
  base::FilePath staging = job->staging;
  jobs_[key] = std::move(job);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&PrepareStaging, staging),
      base::BindOnce(
          [](base::WeakPtr<PackInstaller> self, Key key, bool ok) {
            if (!self) {
              return;
            }
            if (ok) {
              self->Next(key);
            } else {
              self->Finish(key, false);
            }
          },
          weak_factory_.GetWeakPtr(), key));
}

void PackInstaller::Next(Key key) {
  Job& job = *jobs_.at(key);
  if (job.next == job.items.size()) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&MoveIntoPlace, job.staging, job.destination),
        base::BindOnce(&PackInstaller::OnMoved, weak_factory_.GetWeakPtr(),
                       key));
    return;
  }
  const Job::Item& item = job.items[job.next];
  job.loader = MakeLoader(GURL(job.attachments + item.file.location));
  job.loader->SetAllowHttpErrorResults(true);
  job.loader->SetOnDownloadProgressCallback(base::BindRepeating(
      &PackInstaller::ReportProgress, weak_factory_.GetWeakPtr(), key));
  // To a file: models are tens of MB, past what a string download takes.
  job.loader->DownloadToTempFile(
      url_loader_factory().get(),
      base::BindOnce(&PackInstaller::OnDownloaded, weak_factory_.GetWeakPtr(),
                     key),
      item.file.size + 1);
}

void PackInstaller::OnDownloaded(Key key, base::FilePath download) {
  Job& job = *jobs_.at(key);
  const int code = ResponseCode(*job.loader);
  const Job::Item& item = job.items[job.next];
  job.loader.reset();
  if (!download.empty() && code != 200) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::GetDeleteFileCallback(download));
  }
  if (download.empty() || code != 200) {
    LOG(ERROR) << "Translation model download failed (" << code
               << "): " << item.file.location;
    Finish(key, false);
    return;
  }
  base::FilePath destination = job.staging.AppendASCII(item.direction)
                                   .AppendASCII(FileName(item.file.type));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&CheckAndWrite, download, item.file, destination),
      base::BindOnce(&PackInstaller::OnWritten, weak_factory_.GetWeakPtr(),
                     key));
}

void PackInstaller::OnWritten(Key key, bool ok) {
  if (!ok) {
    Finish(key, false);
    return;
  }
  Job& job = *jobs_.at(key);
  job.done += job.items[job.next].file.size;
  ReportProgress(key, 0);
  ++job.next;
  Next(key);
}

void PackInstaller::OnMoved(Key key, bool ok) {
  if (!ok) {
    Finish(key, false);
    return;
  }
  Job& job = *jobs_.at(key);
  const auto& config = GetLanguagePackComponentConfig(key);
  job.local_state->SetFilePath(GetComponentPathPrefName(config),
                               job.destination);
  job.local_state->SetBoolean(GetRegisteredFlagPrefName(config), true);
  Finish(key, true);
}

void PackInstaller::Finish(Key key, bool ok) {
  std::unique_ptr<Job> job = std::move(jobs_.at(key));
  jobs_.erase(key);
  if (ok) {
    for (base::RepeatingClosure& on_ready : job->on_ready) {
      if (on_ready) {
        on_ready.Run();
      }
    }
  } else {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::GetDeletePathRecursivelyCallback(job->staging));
  }
  Report(key, ok);
}

void PackInstaller::OnInstalled(Key key,
                                base::OnceCallback<void(bool)> callback) {
  waiters_[key].push_back(std::move(callback));
}

void PackInstaller::OnProgress(
    Key key,
    base::RepeatingCallback<void(int64_t, int64_t)> callback) {
  progress_[key].push_back(std::move(callback));
}

void PackInstaller::ReportProgress(Key key, uint64_t current_file_bytes) {
  auto job = jobs_.find(key);
  auto callbacks = progress_.find(key);
  if (job == jobs_.end() || callbacks == progress_.end()) {
    return;
  }
  const int64_t received =
      std::min(job->second->done + static_cast<int64_t>(current_file_bytes),
               job->second->total);
  for (const auto& callback : callbacks->second) {
    callback.Run(received, job->second->total);
  }
}

void PackInstaller::Report(Key key, bool ok) {
  progress_.erase(key);
  if (auto it = waiters_.find(key); it != waiters_.end()) {
    std::vector<base::OnceCallback<void(bool)>> waiters = std::move(it->second);
    waiters_.erase(it);
    for (auto& waiter : waiters) {
      std::move(waiter).Run(ok);
    }
  }
  if (result_callback_) {
    result_callback_.Run(key, ok);
  }
}

void PackInstaller::Uninstall(Key key, PrefService* local_state) {
  const auto& config = GetLanguagePackComponentConfig(key);
  local_state->SetBoolean(GetRegisteredFlagPrefName(config), false);
  local_state->SetFilePath(GetComponentPathPrefName(config), base::FilePath());
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
      base::GetDeletePathRecursivelyCallback(
          packs_dir().AppendASCII(GetPackageInstallDirName(key))));
}

scoped_refptr<network::SharedURLLoaderFactory>
PackInstaller::url_loader_factory() {
  if (factory_for_testing_) {
    return factory_for_testing_;
  }
  return g_browser_process->shared_url_loader_factory();
}

const Manifest& PackInstaller::manifest() const {
  return manifest_for_testing_ ? *manifest_for_testing_ : BuiltInManifest();
}

void PackInstaller::SetForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    const Manifest* manifest,
    base::FilePath packs_dir) {
  factory_for_testing_ = std::move(factory);
  manifest_for_testing_ = manifest;
  packs_dir_for_testing_ = std::move(packs_dir);
}

void PackInstaller::SetResultCallbackForTesting(
    base::RepeatingCallback<void(Key, bool)> callback) {
  result_callback_ = std::move(callback);
}

}  // namespace compass::translate
