// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_FIND_BAR_FIND_BAR_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_FIND_BAR_FIND_BAR_UI_H_

#include <string_view>

#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "compass/browser/ui/webui/find_bar/find_bar.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace compass {

inline constexpr char kCompassFindBarHost[] = "compass-find";
inline constexpr char kCompassFindBarURL[] = "chrome://compass-find/";

class FindBarUI;

class FindBarUIConfig : public content::DefaultWebUIConfig<FindBarUI> {
 public:
  FindBarUIConfig();
};

// chrome://compass-find: the find bar's page, shown by compass::FindBar over
// the page's top corner. A TopChromeWebUIController, as the sidebar: the
// browser's own UI, at 100% zoom. See docs/design/claude.md, "Find bar".
class FindBarUI : public TopChromeWebUIController,
                  public find_bar::mojom::PageHandlerFactory {
 public:
  explicit FindBarUI(content::WebUI* web_ui);
  FindBarUI(const FindBarUI&) = delete;
  FindBarUI& operator=(const FindBarUI&) = delete;
  ~FindBarUI() override;

  void BindInterface(
      mojo::PendingReceiver<find_bar::mojom::PageHandlerFactory> receiver);

  static constexpr std::string_view GetWebUIName() {
    return "CompassFindBar";
  }

 private:
  // find_bar::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<find_bar::mojom::Page> page,
      mojo::PendingReceiver<find_bar::mojom::PageHandler> receiver) override;

  mojo::Receiver<find_bar::mojom::PageHandlerFactory> page_factory_receiver_{
      this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_FIND_BAR_FIND_BAR_UI_H_
