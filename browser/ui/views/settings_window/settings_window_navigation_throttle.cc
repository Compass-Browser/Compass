// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/settings_window/settings_window_navigation_throttle.h"

#include <memory>
#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "compass/browser/ui/settings_window_hooks.h"
#include "compass/browser/ui/views/settings_window/settings_window.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The window's path for |url|, if the window shows it.
std::optional<std::string> WindowPathFor(const GURL& url) {
  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return std::nullopt;
  }
  std::string sub_page(url.path().substr(1));
  if (url.has_query()) {
    base::StrAppend(&sub_page, {"?", url.query()});
  }
  if (url.host() == chrome::kChromeUISettingsHost) {
    return SettingsWindowPathFor(sub_page);
  }
  // The window's own page, but not the standalone sections
  // (chrome://compass-settings/boosts.html), which tests load in tabs.
  if (url.host() == kSettingsHost && !url.path().ends_with(".html")) {
    return sub_page;
  }
  return std::nullopt;
}

void ShowWindowFor(base::WeakPtr<content::WebContents> contents,
                   std::string path,
                   bool close_tab) {
  if (!contents) {
    return;
  }
  SettingsWindow::Show(
      Profile::FromBrowserContext(contents->GetBrowserContext()), path);
  if (close_tab) {
    contents->ClosePage();
  }
}

}  // namespace

void MaybeAddSettingsWindowThrottle(
    content::NavigationThrottleRegistry& registry) {
  SettingsWindowNavigationThrottle::MaybeCreateAndAdd(registry);
}

// static
void SettingsWindowNavigationThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  if (!handle.IsInPrimaryMainFrame() || !WindowPathFor(handle.GetURL())) {
    return;
  }
  registry.AddThrottle(
      std::make_unique<SettingsWindowNavigationThrottle>(registry));
}

SettingsWindowNavigationThrottle::SettingsWindowNavigationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

SettingsWindowNavigationThrottle::~SettingsWindowNavigationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
SettingsWindowNavigationThrottle::WillStartRequest() {
  content::WebContents* contents = navigation_handle()->GetWebContents();
  std::optional<std::string> path =
      WindowPathFor(navigation_handle()->GetURL());
  // The window's own page loads in the window.
  if (!path || SettingsWindow::FromWebContents(contents)) {
    return PROCEED;
  }
  // A tab with nothing in it yet (opened for this) closes.
  content::NavigationEntry* last =
      contents->GetController().GetLastCommittedEntry();
  const bool close_tab = !last || last->IsInitialEntry();
  // Not now: the navigation is still being started.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&ShowWindowFor, contents->GetWeakPtr(),
                                std::move(*path), close_tab));
  return CANCEL_AND_IGNORE;
}

const char* SettingsWindowNavigationThrottle::GetNameForLogging() {
  return "SettingsWindowNavigationThrottle";
}

}  // namespace compass
