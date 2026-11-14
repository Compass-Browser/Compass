// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/reader/reader_untrusted_ui.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "compass/browser/reader/reader_assets.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace compass {

namespace {

constexpr std::string_view kArticlePrefix = "article/";
constexpr std::string_view kArticleSuffix = ".json";

uintptr_t ContextKey(const content::BrowserContext* context) {
  return reinterpret_cast<uintptr_t>(context);
}

scoped_refptr<base::RefCountedMemory> Bytes(std::string_view text) {
  return base::MakeRefCounted<base::RefCountedString>(std::string(text));
}

// The file |path| names (the page's request path, which may carry a query).
void ServeReaderFile(uintptr_t context,
                     const std::string& path,
                     content::WebUIDataSource::GotDataCallback callback) {
  std::string_view file = path;
  file = file.substr(0, file.find_first_of("?#"));
  if (file.empty() || file == "reader.html") {
    std::move(callback).Run(Bytes(reader::assets::kReaderHtml));
  } else if (file == "reader.css") {
    std::move(callback).Run(Bytes(reader::assets::kReaderCss));
  } else if (file == "reader.js") {
    std::move(callback).Run(Bytes(reader::assets::kReaderJs));
  } else if (file == "reader_view.js") {
    std::move(callback).Run(Bytes(reader::assets::kReaderViewJs));
  } else if (file.starts_with(kArticlePrefix) &&
             file.ends_with(kArticleSuffix)) {
    std::string_view token = file.substr(
        kArticlePrefix.size(),
        file.size() - kArticlePrefix.size() - kArticleSuffix.size());
    const std::string* json = ReaderArticleStore::Get().Find(
        reinterpret_cast<const content::BrowserContext*>(context), token);
    std::move(callback).Run(json ? Bytes(*json) : nullptr);
  } else {
    std::move(callback).Run(nullptr);
  }
}

}  // namespace

// static
ReaderArticleStore& ReaderArticleStore::Get() {
  static base::NoDestructor<ReaderArticleStore> store;
  return *store;
}

ReaderArticleStore::ReaderArticleStore() = default;
ReaderArticleStore::~ReaderArticleStore() = default;

std::string ReaderArticleStore::Add(const content::BrowserContext* context,
                                    std::string json) {
  std::string token = base::HexEncodeLower(base::RandBytesAsVector(16));
  entries_[token] = Entry{ContextKey(context), std::move(json)};
  return token;
}

void ReaderArticleStore::Remove(const std::string& token) {
  entries_.erase(token);
}

const std::string* ReaderArticleStore::Find(
    const content::BrowserContext* context,
    std::string_view token) const {
  auto it = entries_.find(token);
  if (it == entries_.end() || it->second.context != ContextKey(context)) {
    return nullptr;
  }
  return &it->second.json;
}

GURL GetReaderPageUrl(const std::string& token) {
  return GURL(base::StrCat({kReaderUrl, "?article=", kArticlePrefix, token,
                            kArticleSuffix}));
}

ReaderUntrustedUIConfig::ReaderUntrustedUIConfig()
    : DefaultWebUIConfig(content::kChromeUIUntrustedScheme, kReaderHost) {}

ReaderUntrustedUI::ReaderUntrustedUI(content::WebUI* web_ui)
    : ui::UntrustedWebUIController(web_ui) {
  content::BrowserContext* context =
      web_ui->GetWebContents()->GetBrowserContext();
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(context, kReaderUrl);
  // Everything comes from the strings built in, or from the store.
  source->SetRequestFilter(
      base::BindRepeating([](const std::string& path) { return true; }),
      base::BindRepeating(&ServeReaderFile, ContextKey(context)));

  using Directive = network::mojom::CSPDirectiveName;
  source->OverrideContentSecurityPolicy(Directive::DefaultSrc,
                                        "default-src 'none';");
  // reader.js and reader_view.js; nothing inline, no eval.
  source->OverrideContentSecurityPolicy(Directive::ScriptSrc,
                                        "script-src 'self';");
  source->OverrideContentSecurityPolicy(Directive::StyleSrc,
                                        "style-src 'self';");
  // The article's images, from anywhere on the web.
  source->OverrideContentSecurityPolicy(Directive::ImgSrc,
                                        "img-src https: http: data:;");
  // The article's JSON.
  source->OverrideContentSecurityPolicy(Directive::ConnectSrc,
                                        "connect-src 'self';");
  source->OverrideContentSecurityPolicy(Directive::FrameSrc,
                                        "frame-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::ObjectSrc,
                                        "object-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::FormAction,
                                        "form-action 'none';");
  source->OverrideContentSecurityPolicy(Directive::FrameAncestors,
                                        "frame-ancestors 'none';");
  // reader_view.js puts sanitized HTML in with innerHTML; the script-src
  // above keeps anything in it from running.
  source->DisableTrustedTypesCSP();
}

ReaderUntrustedUI::~ReaderUntrustedUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(ReaderUntrustedUI)

}  // namespace compass
