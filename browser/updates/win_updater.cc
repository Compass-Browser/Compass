// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/win_updater.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/upgrade_detector/upgrade_detector.h"
#include "compass/browser/updates/buildflags.h"
#include "compass/browser/updates/update_hooks.h"
#include "compass/browser/updates/update_manifest.h"
#include "compass/browser/updates/update_prefs.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"

// Windows updates: Compass reads its channel's manifest, downloads the newer
// installer (Chromium's mini_installer), checks its size, hash and Ed25519
// signature, and runs it. The installer updates in place: with the browser
// running it leaves new_chrome.exe and the new version folder, upstream's
// installed-version monitor sees the new version in the registry, and the
// app menu offers to relaunch, which swaps the executable
// (docs/design/updates.md, "Windows").
namespace compass::updates {
namespace {

constexpr base::TimeDelta kFirstCheckDelay = base::Minutes(2);
constexpr base::TimeDelta kCheckInterval = base::Hours(4);
constexpr size_t kMaxManifestSize = 64 * 1024;
constexpr base::TimeDelta kInstallTimeout = base::Minutes(15);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_updates", R"(
        semantics {
          sender: "Compass updates"
          description:
            "Compass checks its update server for a newer version on the "
            "update channel the user picked, and downloads its installer."
          trigger:
            "Two minutes after launch, every four hours, when the channel "
            "changes, and when the About page opens."
          data: "None: a plain GET of the channel's manifest or installer."
          destination: OTHER
          destination_other: "updates.compassbrowser.org, Compass's update "
                             "server (manifests, older installers), and "
                             "github.com (Compass's releases: installers)."
        }
        policy {
          cookies_allowed: NO
          setting: "None: Compass always updates itself."
          policy_exception_justification: "Security updates."
        })");

std::unique_ptr<network::SimpleURLLoader> MakeLoader(const GURL& url) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 kTrafficAnnotation);
  loader->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  return loader;
}

// Thread pool: the download matches the manifest entry.
bool VerifyFile(const base::FilePath& path, const ManifestEntry& entry) {
  base::MemoryMappedFile mapped;
  if (!mapped.Initialize(path)) {
    return false;
  }
  return VerifyDownload(mapped.bytes(), entry,
                        BUILDFLAG(COMPASS_UPDATE_PUBLIC_KEY));
}

// Thread pool: runs the installer and waits. Returns its exit code, or -1.
int RunInstaller(const base::FilePath& installer) {
  base::CommandLine command(installer);
  // Per-user, like the first install; Compass relaunches when the user asks.
  command.AppendSwitch("do-not-launch-chrome");
  command.AppendSwitch("verbose-logging");
  base::LaunchOptions options;
  options.start_hidden = true;
  base::Process process = base::LaunchProcess(command, options);
  int exit_code = -1;
  if (!process.IsValid() ||
      !process.WaitForExitWithTimeout(kInstallTimeout, &exit_code)) {
    return -1;
  }
  return exit_code;
}

class WinUpdater : public Updater {
 public:
  void Start() {
    channel_.Init(g_browser_process->local_state());
    channel_.Add(kUpdateChannelPref,
                 base::BindRepeating(&WinUpdater::Check,
                                     base::Unretained(this)));
    timer_.Start(FROM_HERE, kFirstCheckDelay,
                 base::BindOnce(&WinUpdater::Check, base::Unretained(this)));
  }

  // Updater:
  void CheckNow(CheckCallback callback) override {
    callback_ = std::move(callback);
    if (UpgradeDetector::GetInstance()->is_upgrade_available()) {
      Report(CheckStatus::kNearlyUpdated);
      return;
    }
    if (busy_) {
      Report(CheckStatus::kUpdating, -1);
      return;
    }
    Check();
  }

  // Chromium's relaunch swaps in new_chrome.exe.
  bool InstallAndRelaunch() override { return false; }

 private:
  void Report(CheckStatus status,
              int progress = 0,
              const std::u16string& message = std::u16string()) {
    if (callback_) {
      callback_.Run(status, progress, message);
    }
  }

  void ScheduleNext() {
    timer_.Start(FROM_HERE, kCheckInterval,
                 base::BindOnce(&WinUpdater::Check, base::Unretained(this)));
  }

  void Check() {
    if (busy_) {
      return;
    }
    busy_ = true;
    timer_.Stop();
    Report(CheckStatus::kChecking);
    const char* channel = IsBetaChannel(*g_browser_process->local_state())
                              ? kChannelBeta
                              : kChannelStable;
    loader_ = MakeLoader(ChannelManifestUrl(channel));
    loader_->DownloadToString(
        g_browser_process->shared_url_loader_factory().get(),
        base::BindOnce(&WinUpdater::OnManifest, base::Unretained(this)),
        kMaxManifestSize);
  }

  void Finish(CheckStatus status,
              const std::u16string& message = std::u16string()) {
    loader_.reset();
    busy_ = false;
    Report(status, 0, message);
    ScheduleNext();
  }

  void OnManifest(std::optional<std::string> body) {
    if (!body) {
      Finish(CheckStatus::kFailed, u"Couldn't reach the update server.");
      return;
    }
    std::optional<ManifestEntry> entry = ParseChannelManifest(*body, "win");
    if (!entry || !IsNewerVersion(entry->version, CompassVersion())) {
      VLOG(1) << "Compass updates: no update";
      Finish(UpgradeDetector::GetInstance()->is_upgrade_available()
                 ? CheckStatus::kNearlyUpdated
                 : CheckStatus::kUpToDate);
      return;
    }
    if (staged_version_ == entry->version) {
      Finish(CheckStatus::kNearlyUpdated);
      return;
    }
    VLOG(1) << "Compass updates: downloading " << entry->version;
    entry_ = *entry;
    base::FilePath dir;
    if (!base::GetTempDir(&dir)) {
      Finish(CheckStatus::kFailed);
      return;
    }
    dir = dir.AppendASCII("CompassUpdate");
    installer_ =
        dir.AppendASCII("CompassSetup-" + entry_.version + ".exe");
    Report(CheckStatus::kUpdating, -1);
    loader_ = MakeLoader(entry_.url);
    loader_->SetOnDownloadProgressCallback(base::BindRepeating(
        &WinUpdater::OnProgress, base::Unretained(this)));
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&base::CreateDirectory, dir),
        base::BindOnce(&WinUpdater::Download, base::Unretained(this)));
  }

  void Download(bool created) {
    if (!created) {
      Finish(CheckStatus::kFailed);
      return;
    }
    loader_->DownloadToFile(
        g_browser_process->shared_url_loader_factory().get(),
        base::BindOnce(&WinUpdater::OnDownloaded, base::Unretained(this)),
        installer_, entry_.size);
  }

  void OnProgress(uint64_t current) {
    if (entry_.size > 0) {
      Report(CheckStatus::kUpdating,
             static_cast<int>(std::min<uint64_t>(100, current * 100 /
                                                          entry_.size)));
    }
  }

  void OnDownloaded(base::FilePath path) {
    if (path.empty()) {
      Finish(CheckStatus::kFailed, u"The update didn't download.");
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&VerifyFile, path, entry_),
        base::BindOnce(&WinUpdater::OnVerified, base::Unretained(this),
                       path));
  }

  void OnVerified(base::FilePath path, bool valid) {
    if (!valid) {
      LOG(ERROR) << "Compass updates: " << entry_.version
                 << " doesn't match its manifest; not installing it";
      base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                                 base::GetDeleteFileCallback(path));
      Finish(CheckStatus::kFailed, u"The update didn't pass its checks.");
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
        base::BindOnce(&RunInstaller, path),
        base::BindOnce(&WinUpdater::OnInstalled, base::Unretained(this),
                       path));
  }

  void OnInstalled(base::FilePath path, int exit_code) {
    VLOG(1) << "Compass updates: installer exited with " << exit_code;
    base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                               base::GetDeleteFileCallback(path));
    if (exit_code < 0) {
      Finish(CheckStatus::kFailed, u"The update didn't install.");
      return;
    }
    // setup.exe's success codes vary (NEW_VERSION_UPDATED, IN_USE_UPDATED,
    // ...); the registry's version is what counts, and upstream's monitor
    // reports it.
    staged_version_ = entry_.version;
    Finish(CheckStatus::kNearlyUpdated);
  }

  PrefChangeRegistrar channel_;
  base::OneShotTimer timer_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  ManifestEntry entry_;
  base::FilePath installer_;
  std::string staged_version_;
  bool busy_ = false;
  CheckCallback callback_;
};

}  // namespace

void StartWinUpdater() {
  static base::NoDestructor<WinUpdater> updater;
  static bool started = false;
  if (started || CompassVersion().empty() ||
      std::string_view(BUILDFLAG(COMPASS_UPDATE_PUBLIC_KEY)).empty()) {
    return;  // Dev builds don't update.
  }
  started = true;
  updater->Start();
  SetUpdater(updater.get());
}

}  // namespace compass::updates
