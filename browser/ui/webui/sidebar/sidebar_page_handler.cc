// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace strata {

SidebarPageHandler::SidebarPageHandler(
    mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver,
    mojo::PendingRemote<sidebar::mojom::Page> page,
    BrowserWindowInterface* browser)
    : browser_(browser),
      receiver_(this, std::move(receiver)),
      page_(std::move(page)) {
  tab_strip_model()->AddObserver(this);
}

SidebarPageHandler::~SidebarPageHandler() = default;

void SidebarPageHandler::GetTabs(GetTabsCallback callback) {
  std::move(callback).Run(GetTabList());
}

void SidebarPageHandler::ActivateTab(int32_t tab_id) {
  const int index = GetIndexOfTab(tab_id);
  if (index != TabStripModel::kNoTab) {
    tab_strip_model()->ActivateTabAt(index);
  }
}

void SidebarPageHandler::CloseTab(int32_t tab_id) {
  const int index = GetIndexOfTab(tab_id);
  if (index != TabStripModel::kNoTab) {
    tab_strip_model()->CloseWebContentsAt(
        index, CLOSE_USER_GESTURE | CLOSE_CREATE_HISTORICAL_TAB);
  }
}

void SidebarPageHandler::NewTab() {
  chrome::NewTab(browser_, NewTabTypes::kNewTabButton);
}

void SidebarPageHandler::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  SendTabs();
}

void SidebarPageHandler::OnTabChangedAt(tabs::TabInterface* tab,
                                        TabChangeType change_type) {
  // Loading-only changes fire on every animation frame; titles and URLs
  // arrive with kAll.
  if (change_type == TabChangeType::kAll) {
    SendTabs();
  }
}

TabStripModel* SidebarPageHandler::tab_strip_model() {
  return browser_->GetTabStripModel();
}

std::vector<sidebar::mojom::TabPtr> SidebarPageHandler::GetTabList() {
  TabStripModel* model = tab_strip_model();
  std::vector<sidebar::mojom::TabPtr> tabs;
  tabs.reserve(model->count());
  for (int i = 0; i < model->count(); ++i) {
    tabs::TabInterface* tab = model->GetTabAtIndex(i);
    content::WebContents* contents = tab->GetContents();
    auto entry = sidebar::mojom::Tab::New();
    entry->id = tab->GetHandle().raw_value();
    entry->title = base::UTF16ToUTF8(contents->GetTitle());
    entry->url = contents->GetLastCommittedURL();
    entry->active = i == model->active_index();
    tabs.push_back(std::move(entry));
  }
  return tabs;
}

int SidebarPageHandler::GetIndexOfTab(int32_t tab_id) {
  tabs::TabInterface* tab = tabs::TabHandle(tab_id).Get();
  return tab ? tab_strip_model()->GetIndexOfTab(tab) : TabStripModel::kNoTab;
}

void SidebarPageHandler::SendTabs() {
  page_->TabsChanged(GetTabList());
}

}  // namespace strata
