// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_READER_READER_UNTRUSTED_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_READER_READER_UNTRUSTED_UI_H_

#include <map>
#include <string>
#include <string_view>

#include "content/public/browser/webui_config.h"
#include "ui/webui/untrusted_web_ui_controller.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace compass {

inline constexpr char kReaderHost[] = "compass-reader";
inline constexpr char kReaderUrl[] = "chrome-untrusted://compass-reader/";

// The articles reader views show, by token: ReaderTabState puts one in
// before it loads its view and takes it out when the view goes. Only the
// profile that put it in can read it.
class ReaderArticleStore {
 public:
  static ReaderArticleStore& Get();

  ReaderArticleStore();
  ReaderArticleStore(const ReaderArticleStore&) = delete;
  ReaderArticleStore& operator=(const ReaderArticleStore&) = delete;
  ~ReaderArticleStore();

  // Keeps |json| for |context|; returns its token, 32 random hex digits.
  std::string Add(const content::BrowserContext* context, std::string json);
  void Remove(const std::string& token);
  // The article |token| names, if |context| put it in.
  const std::string* Find(const content::BrowserContext* context,
                          std::string_view token) const;

 private:
  struct Entry {
    // Compared, never used.
    uintptr_t context;
    std::string json;
  };
  std::map<std::string, Entry, std::less<>> entries_;
};

// The reader view's URL for the article |token| names.
GURL GetReaderPageUrl(const std::string& token);

class ReaderUntrustedUI;

class ReaderUntrustedUIConfig
    : public content::DefaultWebUIConfig<ReaderUntrustedUI> {
 public:
  ReaderUntrustedUIConfig();
};

// chrome-untrusted://compass-reader/: the reader view
// (docs/design/reader-mode.md). Serves common/reader/'s view files and the
// articles in ReaderArticleStore. No Mojo; its CSP runs only its own two
// scripts, so nothing in an article can run.
class ReaderUntrustedUI : public ui::UntrustedWebUIController {
 public:
  explicit ReaderUntrustedUI(content::WebUI* web_ui);
  ReaderUntrustedUI(const ReaderUntrustedUI&) = delete;
  ReaderUntrustedUI& operator=(const ReaderUntrustedUI&) = delete;
  ~ReaderUntrustedUI() override;

 private:
  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_READER_READER_UNTRUSTED_UI_H_
