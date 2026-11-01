// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/empty/compass_empty_ui.h"

#include <memory>
#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/common/webui_url_constants.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/common/referrer.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "compass/browser/ui/views/command_bar/command_bar.h"
#include "compass/browser/window_sync/empty_tab.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "ui/base/page_transition_types.h"
#include "ui/webui/webui_util.h"

namespace compass {

CompassEmptyUIConfig::CompassEmptyUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kEmptyHost) {}

CompassEmptyUI::CompassEmptyUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kEmptyHost);
  // Shares the sidebar's resource pak; only the default page differs.
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_EMPTY_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassEmptyUI::~CompassEmptyUI() = default;

void CompassEmptyUI::BindInterface(
    mojo::PendingReceiver<sidebar::mojom::EmptyPageHandler> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void CompassEmptyUI::ShowCommandBar() {
  content::WebContents* contents = web_ui()->GetWebContents();
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser || browser->GetActiveTabInterface() != tab) {
    return;
  }
  if (CommandBar* bar = CommandBar::From(browser)) {
    if (!bar->IsShowing()) {
      bar->Show(command_bar::mojom::Mode::kNewEntry);
    }
    return;
  }
  // A window without a command bar (a little window): upstream's new tab
  // page, with its search box, is the way to go somewhere.
  contents->GetController().LoadURL(
      GURL(chrome::kChromeUINewTabPageURL), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassEmptyUI)

}  // namespace compass
