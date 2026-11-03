// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/sparkle_updater_mac.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <algorithm>
#include <memory>
#include <string>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version.h"
#include "base/version_info/version_info.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/upgrade_detector/build_state.h"
#include "compass/browser/updates/update_hooks.h"
#include "compass/browser/updates/update_prefs.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"

// Sparkle is loaded at run time (dev builds don't bundle it), so its classes
// are looked up by name and nothing links against the framework; the headers
// only declare the protocols and types.

namespace compass::updates {
class SparkleUpdater;
}

// Sparkle's delegate and user driver. The driver shows nothing: updates check,
// download and wait silently, and Chromium's own UI (the app menu's update
// item, the About page) reports them and relaunches.
@interface CompassSparkleBridge : NSObject <SPUUpdaterDelegate, SPUUserDriver>
- (instancetype)initWithOwner:(compass::updates::SparkleUpdater*)owner;
@end

namespace compass::updates {

// Checks at launch, instead of when Sparkle's schedule says (for testing).
constexpr char kCheckForUpdatesSwitch[] = "compass-check-for-updates";

class SparkleUpdater : public Updater {
 public:
  SparkleUpdater() = default;
  SparkleUpdater(const SparkleUpdater&) = delete;
  SparkleUpdater& operator=(const SparkleUpdater&) = delete;
  ~SparkleUpdater() override = default;

  bool Start() {
    NSBundle* app = base::apple::OuterBundle();
    if (![app objectForInfoDictionaryKey:@"SUFeedURL"] ||
        ![app objectForInfoDictionaryKey:@"SUPublicEDKey"]) {
      return false;  // A dev build: no feed.
    }
    NSString* framework_path = [app.bundlePath
        stringByAppendingPathComponent:@"Contents/Frameworks/Sparkle.framework"];
    NSBundle* framework = [NSBundle bundleWithPath:framework_path];
    NSError* error = nil;
    if (!framework || ![framework loadAndReturnError:&error]) {
      LOG(ERROR) << "Compass updates: can't load Sparkle: "
                 << base::SysNSStringToUTF8(error.description ?: @"missing");
      return false;
    }
    Class updater_class = NSClassFromString(@"SPUUpdater");
    if (!updater_class) {
      return false;
    }
    bridge_ = [[CompassSparkleBridge alloc] initWithOwner:this];
    updater_ = [[updater_class alloc] initWithHostBundle:app
                                       applicationBundle:app
                                              userDriver:bridge_
                                                delegate:bridge_];
    // Sparkle keeps these in the app's user defaults once set; Info.plist
    // sets the defaults (SUEnableAutomaticChecks, SUAutomaticallyUpdate).
    updater_.automaticallyChecksForUpdates = YES;
    updater_.automaticallyDownloadsUpdates = YES;
    updater_.sendsSystemProfile = NO;
    if (![updater_ startUpdater:&error]) {
      LOG(ERROR) << "Compass updates: Sparkle didn't start: "
                 << base::SysNSStringToUTF8(error.description);
      updater_ = nil;
      return false;
    }

    // A new channel takes effect at once: check it now.
    channel_.Init(g_browser_process->local_state());
    channel_.Add(kUpdateChannelPref,
                 base::BindRepeating(&SparkleUpdater::OnChannelChanged,
                                     base::Unretained(this)));
    VLOG(1) << "Compass updates: Sparkle started, feed "
            << base::SysNSStringToUTF8(updater_.feedURL.absoluteString)
            << (beta() ? ", beta" : ", stable");
    if (base::CommandLine::ForCurrentProcess()->HasSwitch(
            kCheckForUpdatesSwitch)) {
      [updater_ checkForUpdatesInBackground];
    }
    return true;
  }

  // Updater:
  void CheckNow(CheckCallback callback) override {
    callback_ = std::move(callback);
    if (install_handler_) {
      Report(CheckStatus::kNearlyUpdated);
      return;
    }
    if (updater_.sessionInProgress) {
      Report(downloading_ ? CheckStatus::kUpdating : CheckStatus::kChecking);
      return;
    }
    Report(CheckStatus::kChecking);
    [updater_ checkForUpdatesInBackground];
  }

  bool InstallAndRelaunch() override {
    if (!install_handler_) {
      return false;
    }
    VLOG(1) << "Compass updates: installing and relaunching";
    // Sparkle's installer replaces the app once it quits, then opens it
    // again; Sparkle asks the app to terminate.
    install_handler_();
    return true;
  }

  // From the bridge (main thread).
  bool beta() const {
    return IsBetaChannel(*g_browser_process->local_state());
  }

  void OnUpdateFound() {
    VLOG(1) << "Compass updates: found an update";
    downloading_ = true;
    Report(CheckStatus::kUpdating);
  }

  void OnNoUpdate() {
    VLOG(1) << "Compass updates: no update";
    downloading_ = false;
    Report(install_handler_ ? CheckStatus::kNearlyUpdated
                            : CheckStatus::kUpToDate);
  }

  void OnProgress(int progress) {
    Report(CheckStatus::kUpdating, progress);
  }

  void OnFailed(NSError* error) {
    downloading_ = false;
    if (error.code == SUNoUpdateError) {
      OnNoUpdate();
      return;
    }
    LOG(WARNING) << "Compass updates: "
                 << base::SysNSStringToUTF8(error.description);
    Report(CheckStatus::kFailed, 0,
           base::SysNSStringToUTF16(error.localizedDescription ?: @""));
  }

  // The update is downloaded and installs on quit; |handler| installs it now
  // and relaunches.
  void OnReadyToInstall(SUAppcastItem* item, void (^handler)(void)) {
    downloading_ = false;
    install_handler_ = handler;
    VLOG(1) << "Compass updates: ready to install "
            << base::SysNSStringToUTF8(item.versionString ?: @"");

    // Upstream's upgrade detector compares Chromium versions. A Compass
    // release can keep the same Chromium, so then report one just above the
    // running version: the app menu offers "Relaunch to update" either way.
    base::Version running = version_info::GetVersion();
    base::Version next(base::SysNSStringToUTF8(item.displayVersionString ?: @""));
    if (!next.IsValid() || next.CompareTo(running) <= 0) {
      next = base::Version(running.GetString() + ".1");
    }
    SetPendingVersion(next);
    g_browser_process->GetBuildState()->SetUpdate(
        BuildState::UpdateType::kNormalUpdate, next, std::nullopt);
    Report(CheckStatus::kNearlyUpdated);
  }

 private:
  void Report(CheckStatus status,
              int progress = 0,
              const std::u16string& message = std::u16string()) {
    if (callback_) {
      callback_.Run(status, progress, message);
    }
  }

  void OnChannelChanged() {
    if (install_handler_ || updater_.sessionInProgress) {
      return;  // The next cycle reads the new channel.
    }
    [updater_ checkForUpdatesInBackground];
  }

  SPUUpdater* __strong updater_ = nil;
  CompassSparkleBridge* __strong bridge_ = nil;
  void (^install_handler_)(void) = nil;
  bool downloading_ = false;
  CheckCallback callback_;
  PrefChangeRegistrar channel_;
};

void StartSparkleUpdater() {
  static base::NoDestructor<SparkleUpdater> updater;
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  if (updater->Start()) {
    SetUpdater(updater.get());
  }
}

}  // namespace compass::updates

@implementation CompassSparkleBridge {
  raw_ptr<compass::updates::SparkleUpdater> _owner;
  uint64_t _expectedLength;
  uint64_t _receivedLength;
  void (^_readyReply)(SPUUserUpdateChoice);
}

- (instancetype)initWithOwner:(compass::updates::SparkleUpdater*)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}

#pragma mark - SPUUpdaterDelegate

- (NSSet<NSString*>*)allowedChannelsForUpdater:(SPUUpdater*)updater {
  // Beta users also get stable releases (untagged items) when they're newer.
  return _owner->beta() ? [NSSet setWithObject:@"beta"] : [NSSet set];
}

- (BOOL)updaterShouldPromptForPermissionToCheckForUpdates:(SPUUpdater*)updater {
  return NO;
}

- (void)updater:(SPUUpdater*)updater didFindValidUpdate:(SUAppcastItem*)item {
  _owner->OnUpdateFound();
}

- (void)updaterDidNotFindUpdate:(SPUUpdater*)updater error:(NSError*)error {
  _owner->OnNoUpdate();
}

- (void)updater:(SPUUpdater*)updater didAbortWithError:(NSError*)error {
  _owner->OnFailed(error);
}

- (void)updater:(SPUUpdater*)updater
    failedToDownloadUpdate:(SUAppcastItem*)item
                     error:(NSError*)error {
  _owner->OnFailed(error);
}

- (BOOL)updater:(SPUUpdater*)updater
    willInstallUpdateOnQuit:(SUAppcastItem*)item
    immediateInstallationBlock:(void (^)(void))immediateInstallHandler {
  _owner->OnReadyToInstall(item, immediateInstallHandler);
  // Compass decides when: the user relaunches from the app menu or the About
  // page. Quitting installs it too.
  return YES;
}

#pragma mark - SPUUserDriver (silent)

- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest*)request
                              reply:(void (^)(SUUpdatePermissionResponse*))
                                        reply {
  Class response_class = NSClassFromString(@"SUUpdatePermissionResponse");
  reply([[response_class alloc] initWithAutomaticUpdateChecks:YES
                                            sendSystemProfile:NO]);
}

- (void)showUserInitiatedUpdateCheckWithCancellation:
    (void (^)(void))cancellation {
}

- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem*)appcastItem
                                 state:(SPUUserUpdateState*)state
                                 reply:(void (^)(SPUUserUpdateChoice))reply {
  // Reached when Sparkle won't update on its own (e.g. a critical update):
  // download it anyway, as an automatic update would.
  _owner->OnUpdateFound();
  reply(SPUUserUpdateChoiceInstall);
}

- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData*)downloadData {
}

- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError*)error {
}

- (void)showUpdateNotFoundWithError:(NSError*)error
                    acknowledgement:(void (^)(void))acknowledgement {
  _owner->OnNoUpdate();
  acknowledgement();
}

- (void)showUpdaterError:(NSError*)error
         acknowledgement:(void (^)(void))acknowledgement {
  _owner->OnFailed(error);
  acknowledgement();
}

- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
  _expectedLength = 0;
  _receivedLength = 0;
  _owner->OnProgress(-1);
}

- (void)showDownloadDidReceiveExpectedContentLength:
    (uint64_t)expectedContentLength {
  _expectedLength = expectedContentLength;
}

- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {
  _receivedLength += length;
  if (_expectedLength > 0) {
    _owner->OnProgress(static_cast<int>(
        std::min<uint64_t>(100, _receivedLength * 100 / _expectedLength)));
  }
}

- (void)showDownloadDidStartExtractingUpdate {
  _owner->OnProgress(-1);
}

- (void)showExtractionReceivedProgress:(double)progress {
}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
  // The user-driver path's equivalent of willInstallUpdateOnQuit: keep the
  // reply, which installs and relaunches when the user asks to.
  _readyReply = reply;
  void (^installNow)(void) = ^{
    if (self->_readyReply) {
      void (^pending)(SPUUserUpdateChoice) = self->_readyReply;
      self->_readyReply = nil;
      pending(SPUUserUpdateChoiceInstall);
    }
  };
  _owner->OnReadyToInstall(nil, installNow);
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)applicationTerminated
                          retryTerminatingApplication:
                              (void (^)(void))retryTerminatingApplication {
}

- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched
                         acknowledgement:(void (^)(void))acknowledgement {
  acknowledgement();
}

- (void)dismissUpdateInstallation {
}

- (void)showUpdateInFocus {
}

@end
