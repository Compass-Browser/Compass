// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/dev_mode/network_log.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace compass::dev_mode {

namespace {

// As Claude's network tool asks (claude_devtools_tools.cc): enough for a
// page's bodies to stay fetchable after DevTools opens.
base::DictValue NetworkEnableParams() {
  return base::DictValue()
      .Set("maxTotalBufferSize", 50 * 1024 * 1024)
      .Set("maxResourceBufferSize", 10 * 1024 * 1024)
      .Set("maxPostDataSize", 64 * 1024);
}

}  // namespace

NetworkLog::NetworkLog(content::WebContents* contents)
    : session_(
          contents,
          base::BindRepeating(&NetworkLog::OnEvent, base::Unretained(this)),
          base::BindRepeating(&NetworkLog::OnAttach, base::Unretained(this))) {
  session_.Attach();
}

NetworkLog::~NetworkLog() = default;

void NetworkLog::Send(const std::string& method,
                      base::DictValue params,
                      claude::DevToolsSession::ResultCallback callback) {
  session_.Send(method, std::move(params), std::move(callback));
}

void NetworkLog::OnAttach() {
  session_.Send("Network.enable", NetworkEnableParams(), base::DoNothing());
}

void NetworkLog::OnEvent(const std::string& method,
                         const base::DictValue& params) {
  buffer_.Add(method, params);
}

DevModeTabHelper::DevModeTabHelper(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<DevModeTabHelper>(*contents) {
  pref_change_registrar_.Init(
      Profile::FromBrowserContext(contents->GetBrowserContext())->GetPrefs());
  pref_change_registrar_.Add(
      prefs::kDevModeHosts,
      base::BindRepeating(&DevModeTabHelper::OnHostsChanged,
                          base::Unretained(this)));
}

DevModeTabHelper::~DevModeTabHelper() = default;

void DevModeTabHelper::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInPrimaryMainFrame() ||
      navigation_handle->IsSameDocument()) {
    return;
  }
  // Before the navigation's own request, so it's the log's first.
  if (network_log_) {
    network_log_->Clear();
  }
  Update(navigation_handle->GetURL());
}

void DevModeTabHelper::Update(const GURL& url) {
  if (!IsEnabledFor(pref_change_registrar_.prefs(), url)) {
    network_log_.reset();
  } else if (!network_log_) {
    network_log_ = std::make_unique<NetworkLog>(web_contents());
  }
}

void DevModeTabHelper::OnHostsChanged() {
  // Turned on for the page: records from now (a reload shows the page's
  // first requests too).
  Update(web_contents()->GetLastCommittedURL());
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(DevModeTabHelper);

void AttachDevModeTabHelper(content::WebContents* contents) {
  DevModeTabHelper::CreateForWebContents(contents);
}

}  // namespace compass::dev_mode
