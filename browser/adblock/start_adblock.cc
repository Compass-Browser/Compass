// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/time/default_clock.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/chrome_paths.h"
#include "compass/browser/adblock/adblock_hooks.h"
#include "compass/browser/adblock/adblock_service.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace compass::adblock {

void StartAdBlock() {
  if (AdBlockService::Get()) {
    return;
  }
  base::FilePath user_data_dir;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);
  // Lives as long as the browser process (its prefs and the ruleset service
  // are the browser process's).
  static base::NoDestructor<AdBlockService> service(
      g_browser_process->local_state(), user_data_dir,
      g_browser_process->shared_url_loader_factory(),
      g_browser_process->subresource_filter_ruleset_service(),
      base::DefaultClock::GetInstance());
  service->Start();
}

}  // namespace compass::adblock
