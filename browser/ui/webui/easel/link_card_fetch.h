// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EASEL_LINK_CARD_FETCH_H_
#define COMPASS_BROWSER_UI_WEBUI_EASEL_LINK_CARD_FETCH_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/easels/easel_images.h"
#include "compass/browser/ui/webui/easel/easel.mojom.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
struct ResourceRequest;
namespace mojom {
class URLResponseHead;
}  // namespace mojom
}  // namespace network

namespace compass::easels {
class EaselStore;
}  // namespace compass::easels

namespace compass::easel {

// A link card for a pasted or dropped URL (docs/design/easels.md, "link"):
// reads the first 1 MB of the page in the browser, without cookies, for its
// title, description and preview image, then fetches the image and keeps it
// in the store. Whatever fails, the card still has its URL.
class LinkCardFetch : public network::SimpleURLLoaderStreamConsumer {
 public:
  using Done = base::OnceCallback<void(mojom::LinkContentPtr)>;

  // The page's bytes read at most; a head is in there.
  static constexpr size_t kMaxPageBytes = 1024 * 1024;
  // A preview image bigger than this isn't fetched.
  static constexpr size_t kMaxImageBytes = 5 * 1024 * 1024;

  // |url| must be http or https.
  LinkCardFetch(const GURL& url,
                scoped_refptr<network::SharedURLLoaderFactory> factory,
                base::WeakPtr<easels::EaselStore> store,
                Done done);
  LinkCardFetch(const LinkCardFetch&) = delete;
  LinkCardFetch& operator=(const LinkCardFetch&) = delete;
  ~LinkCardFetch() override;

  void Start();

 private:
  std::unique_ptr<network::ResourceRequest> MakeRequest(const GURL& url) const;
  void OnResponseStarted(const GURL& final_url,
                         const network::mojom::URLResponseHead& head);

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view string_piece,
                      base::OnceClosure resume) override;
  void OnComplete(bool success) override;
  void OnRetry(base::OnceClosure start_retry) override;

  void OnPageRead();
  void OnImageDownloaded(std::optional<std::string> body);
  void OnImagePrepared(
      base::expected<easels::PreparedImage, std::string> image);
  void Finish();

  const GURL url_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  base::WeakPtr<easels::EaselStore> store_;
  Done done_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  GURL final_url_;
  std::string charset_;
  bool is_html_ = false;
  bool page_read_ = false;
  std::string body_;
  mojom::LinkContentPtr link_;
  base::WeakPtrFactory<LinkCardFetch> weak_factory_{this};
};

}  // namespace compass::easel

#endif  // COMPASS_BROWSER_UI_WEBUI_EASEL_LINK_CARD_FETCH_H_
