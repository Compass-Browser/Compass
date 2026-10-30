// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/placeholder/compass_placeholder_ui.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "ui/webui/webui_util.h"

namespace compass {

CompassPlaceholderUIConfig::CompassPlaceholderUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kPlaceholderHost) {}

CompassPlaceholderUI::CompassPlaceholderUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kPlaceholderHost);
  // Shares the sidebar's resource pak; only the default page differs.
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_PLACEHOLDER_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassPlaceholderUI::~CompassPlaceholderUI() = default;

void CompassPlaceholderUI::BindInterface(
    mojo::PendingReceiver<sidebar::mojom::PlaceholderHandler> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void CompassPlaceholderUI::GetInfo(GetInfoCallback callback) {
  SidebarNode* node = GetNode();
  if (!node) {
    std::move(callback).Run(std::string(), GURL());
    return;
  }
  std::move(callback).Run(base::UTF16ToUTF8(node->title()), node->url());
}

void CompassPlaceholderUI::GetSnapshot(GetSnapshotCallback callback) {
  TabStripBridge* bridge = GetBridge();
  if (!bridge) {
    std::move(callback).Run(std::string());
    return;
  }
  bridge->GetSnapshot(
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents()),
      base::BindOnce(
          [](GetSnapshotCallback callback, const std::string& data_url) {
            std::move(callback).Run(data_url);
          },
          std::move(callback)));
}

void CompassPlaceholderUI::BringHere() {
  tabs::TabInterface* placeholder =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  SidebarNode* node = GetNode();
  if (!node) {
    return;
  }
  BrowserWindowInterface* browser = placeholder->GetBrowserWindowInterface();
  // The tab comes in active; the bridge then closes this placeholder, which
  // destroys |this|.
  if (tabs::TabInterface* tab =
          WindowFeatures::From(browser)->tab_strip_bridge().OpenNode(node)) {
    TabStripModel* model = browser->GetTabStripModel();
    model->ActivateTabAt(model->GetIndexOfTab(tab));
  }
}

SidebarNode* CompassPlaceholderUI::GetNode() {
  TabStripBridge* bridge = GetBridge();
  return bridge ? bridge->GetPlaceholderNode(
                      tabs::TabInterface::MaybeGetFromContents(
                          web_ui()->GetWebContents()))
                : nullptr;
}

TabStripBridge* CompassPlaceholderUI::GetBridge() {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  WindowFeatures* features = browser ? WindowFeatures::From(browser) : nullptr;
  return features ? &features->tab_strip_bridge() : nullptr;
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassPlaceholderUI)

}  // namespace compass
