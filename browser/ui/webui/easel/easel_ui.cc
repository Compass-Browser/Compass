// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/easel/easel_ui.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/ui/webui/easel/easel_page_handler.h"
#include "compass/browser/ui/webui/localized_strings/localized_strings.h"
#include "compass/grit/compass_easel_resources.h"
#include "compass/grit/compass_easel_resources_map.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/webui_util.h"

namespace compass::easel {
namespace {

constexpr std::string_view kAssetPrefix = "asset/";

bool IsAssetRequest(const std::string& path) {
  return path.starts_with(kAssetPrefix);
}

// "asset/<sha256>", maybe with a query or an extension the page added.
void ServeAsset(base::WeakPtr<easels::EaselStore> store,
                const std::string& path,
                content::WebUIDataSource::GotDataCallback callback) {
  std::string_view sha256 = std::string_view(path).substr(kAssetPrefix.size());
  sha256 = sha256.substr(0, sha256.find_first_of("?#."));
  const sync_pb::CompassEaselAsset* asset =
      store ? store->FindAsset(sha256) : nullptr;
  if (!asset) {
    std::move(callback).Run(nullptr);
    return;
  }
  // The data source names types by extension, so this goes as text/html;
  // <img> reads images by their bytes, and nosniff doesn't apply to them.
  std::move(callback).Run(
      base::MakeRefCounted<base::RefCountedString>(asset->data()));
}

}  // namespace

EaselUIConfig::EaselUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kEaselHost) {}

EaselUI::EaselUI(content::WebUI* web_ui) : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kEaselHost);
  // Every other path (a board's uuid) is the page; it reads the board from
  // its URL.
  webui::SetupWebUIDataSource(source, kCompassEaselResources,
                              IDR_COMPASS_EASEL_EASEL_HTML);
  AddEaselLocalizedStrings(source);
  easels::EaselStore* store = easels::EaselStore::Get(profile);
  source->SetRequestFilter(
      base::BindRepeating(&IsAssetRequest),
      base::BindRepeating(&ServeAsset,
                          store ? store->GetWeakPtr()
                                : base::WeakPtr<easels::EaselStore>()));
}

EaselUI::~EaselUI() = default;

void EaselUI::BindInterface(
    mojo::PendingReceiver<mojom::PageHandlerFactory> receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void EaselUI::CreatePageHandler(
    mojo::PendingRemote<mojom::Page> page,
    mojo::PendingReceiver<mojom::PageHandler> receiver) {
  page_handler_ = std::make_unique<EaselPageHandler>(
      std::move(receiver), std::move(page), Profile::FromWebUI(web_ui()),
      web_ui()->GetWebContents());
}

WEB_UI_CONTROLLER_TYPE_IMPL(EaselUI)

}  // namespace compass::easel
