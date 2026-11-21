// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_DEV_MODE_NETWORK_LOG_H_
#define COMPASS_BROWSER_DEV_MODE_NETWORK_LOG_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "compass/browser/claude/devtools_session.h"
#include "compass/browser/dev_mode/network_log_buffer.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

class GURL;

namespace compass::dev_mode {

// The requests of a page in development mode, recorded whether DevTools is
// open or not (docs/design/dev-mode.md, "Requests before DevTools"), by an
// internal DevTools session with the Network domain on. Its requests'
// response bodies stay fetchable through Send().
class NetworkLog {
 public:
  explicit NetworkLog(content::WebContents* contents);
  NetworkLog(const NetworkLog&) = delete;
  NetworkLog& operator=(const NetworkLog&) = delete;
  ~NetworkLog();

  const NetworkLogBuffer& buffer() const { return buffer_; }
  // A new page: what's recorded so far goes, as DevTools' panel clears
  // without "Preserve log".
  void Clear() { buffer_.Clear(); }

  // Sends a command on the recording session (Network.getResponseBody for a
  // recorded request).
  void Send(const std::string& method,
            base::DictValue params,
            claude::DevToolsSession::ResultCallback callback);

 private:
  void OnAttach();
  void OnEvent(const std::string& method, const base::DictValue& params);

  NetworkLogBuffer buffer_;
  claude::DevToolsSession session_;
};

// Every tab's development mode helper on the desktop: records the tab's
// requests (NetworkLog) while its page is on a site in development mode.
// Attached by upstream's tab helpers (see
// patches/chrome-browser-ui-tab_helpers.cc.patch).
class DevModeTabHelper : public content::WebContentsObserver,
                         public content::WebContentsUserData<DevModeTabHelper> {
 public:
  DevModeTabHelper(const DevModeTabHelper&) = delete;
  DevModeTabHelper& operator=(const DevModeTabHelper&) = delete;
  ~DevModeTabHelper() override;

  // The tab's log, while its page is in development mode.
  NetworkLog* network_log() { return network_log_.get(); }

  // content::WebContentsObserver:
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override;

 private:
  friend class content::WebContentsUserData<DevModeTabHelper>;

  explicit DevModeTabHelper(content::WebContents* contents);

  // Starts or stops recording for a page at |url|.
  void Update(const GURL& url);
  void OnHostsChanged();

  std::unique_ptr<NetworkLog> network_log_;
  PrefChangeRegistrar pref_change_registrar_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

// Gives |contents| its DevModeTabHelper.
void AttachDevModeTabHelper(content::WebContents* contents);

}  // namespace compass::dev_mode

#endif  // COMPASS_BROWSER_DEV_MODE_NETWORK_LOG_H_
