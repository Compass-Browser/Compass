// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/easel/link_card_fetch.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/icu_string_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/ui/webui/easel/link_metadata.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::easel {
namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_easel_link_card", R"(
        semantics {
          sender: "Compass Easels"
          description:
            "Reads a web page's title, description and preview image "
            "(Open Graph tags), and fetches that image, to show the page as "
            "a link card on an Easel."
          trigger: "The user pastes or drops a link onto an Easel."
          data: "The link's URL, and then its preview image's URL. No "
                "cookies or credentials."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          setting: "Only when the user adds a link to an Easel."
          policy_exception_justification: "Not implemented."
        })");

constexpr base::TimeDelta kTimeout = base::Seconds(15);

// The page's text as UTF-8: by the response's charset, else the head's
// <meta charset>, else UTF-8 if it is, else Windows-1252 (as browsers do).
std::string DecodePage(const std::string& bytes, std::string charset) {
  if (charset.empty()) {
    charset = FindMetaCharset(bytes);
  }
  std::string decoded;
  if (!charset.empty() && !base::EqualsCaseInsensitiveASCII(charset, "utf-8") &&
      base::ConvertToUtf8AndNormalize(bytes, charset, &decoded)) {
    return decoded;
  }
  if (base::IsStringUTF8AllowingNoncharacters(bytes)) {
    return bytes;
  }
  if (base::ConvertToUtf8AndNormalize(bytes, "windows-1252", &decoded)) {
    return decoded;
  }
  // Invalid UTF-8 becomes U+FFFD.
  return base::UTF16ToUTF8(base::UTF8ToUTF16(bytes));
}

}  // namespace

LinkCardFetch::LinkCardFetch(
    const GURL& url,
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    base::WeakPtr<easels::EaselStore> store,
    Done done)
    : url_(url),
      factory_(std::move(factory)),
      store_(std::move(store)),
      done_(std::move(done)),
      final_url_(url),
      link_(mojom::LinkContent::New(url.spec(),
                                    std::string(),
                                    std::string(),
                                    std::string())) {}

LinkCardFetch::~LinkCardFetch() = default;

std::unique_ptr<network::ResourceRequest> LinkCardFetch::MakeRequest(
    const GURL& url) const {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "GET";
  // A preview, not a visit: no cookies, so a site doesn't learn who added
  // its link, and nothing is set.
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  return request;
}

void LinkCardFetch::Start() {
  loader_ = network::SimpleURLLoader::Create(MakeRequest(url_),
                                             kTrafficAnnotation);
  loader_->SetTimeoutDuration(kTimeout);
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &LinkCardFetch::OnResponseStarted, weak_factory_.GetWeakPtr()));
  loader_->DownloadAsStream(factory_.get(), this);
}

void LinkCardFetch::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  final_url_ = final_url;
  charset_ = head.charset;
  is_html_ = head.mime_type.empty() || head.mime_type == "text/html" ||
             head.mime_type == "application/xhtml+xml";
  if (!is_html_) {
    // A PDF, an image, a download: a card with the URL.
    loader_.reset();
    Finish();
  }
}

void LinkCardFetch::OnDataReceived(std::string_view string_piece,
                                   base::OnceClosure resume) {
  body_.append(string_piece.substr(
      0, std::min(string_piece.size(), kMaxPageBytes - body_.size())));
  if (body_.size() >= kMaxPageBytes) {
    // Enough: the head is at the start.
    loader_.reset();
    OnPageRead();
    return;
  }
  std::move(resume).Run();
}

void LinkCardFetch::OnComplete(bool success) {
  loader_.reset();
  // A page that failed halfway may still have given its head.
  OnPageRead();
}

void LinkCardFetch::OnRetry(base::OnceClosure start_retry) {
  body_.clear();
  std::move(start_retry).Run();
}

void LinkCardFetch::OnPageRead() {
  if (page_read_) {
    return;
  }
  page_read_ = true;
  if (body_.empty()) {
    Finish();
    return;
  }
  const LinkMetadata metadata =
      ParseLinkMetadata(DecodePage(body_, charset_), final_url_);
  body_.clear();
  link_->title = metadata.title;
  link_->description = metadata.description;
  if (!metadata.image_url.is_valid() || !store_) {
    Finish();
    return;
  }
  loader_ = network::SimpleURLLoader::Create(MakeRequest(metadata.image_url),
                                             kTrafficAnnotation);
  loader_->SetTimeoutDuration(kTimeout);
  loader_->DownloadToString(
      factory_.get(),
      base::BindOnce(&LinkCardFetch::OnImageDownloaded,
                     weak_factory_.GetWeakPtr()),
      kMaxImageBytes);
}

void LinkCardFetch::OnImageDownloaded(std::optional<std::string> body) {
  loader_.reset();
  if (!body || body->empty()) {
    Finish();
    return;
  }
  easels::PrepareImage(std::move(*body),
               base::BindOnce(&LinkCardFetch::OnImagePrepared,
                              weak_factory_.GetWeakPtr()));
}

void LinkCardFetch::OnImagePrepared(
    base::expected<easels::PreparedImage, std::string> image) {
  if (image.has_value() && store_) {
    base::expected<std::string, std::string> sha256 =
        store_->AddAsset(std::move(image->data), std::move(image->mime_type));
    if (sha256.has_value()) {
      link_->image_asset = std::move(*sha256);
    }
  }
  Finish();
}

void LinkCardFetch::Finish() {
  // Later, as the loader's callbacks may still be on the stack and |done_|
  // may delete this.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<LinkCardFetch> fetch) {
                       if (fetch && fetch->done_) {
                         std::move(fetch->done_).Run(std::move(fetch->link_));
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

}  // namespace compass::easel
