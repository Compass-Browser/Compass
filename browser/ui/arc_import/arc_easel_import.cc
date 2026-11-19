// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/arc_import/arc_easel_import.h"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/arc_import/sidebar_import.h"
#include "compass/browser/easels/easel_images.h"
#include "compass/browser/easels/easel_store.h"
#include "content/public/browser/storage_partition.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::arc {

namespace {

// PrepareImage() refuses more than 32 MB; Arc's images are much smaller.
constexpr size_t kMaxImageBytes = 20 * 1024 * 1024;
constexpr size_t kMaxDownloads = 4;
constexpr base::TimeDelta kDownloadTimeout = base::Seconds(60);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_arc_easel_images", R"(
        semantics {
          sender: "Compass Arc import"
          description:
            "Downloads the images of the user's Arc Easels (pictures, link "
            "previews, page captures and their icons) to bring the boards "
            "over to Compass."
          trigger: "The user imports from Arc and has Easels."
          data: "None. The image URLs are the ones Arc stored; each "
                "carries its own download token."
          destination: OTHER
          destination_other: "Firebase Storage (Arc's image storage)."
        }
        policy {
          cookies_allowed: NO
          setting: "Only when the user imports from Arc."
          policy_exception_justification: "Not implemented."
        })");

// An image kept in the store.
struct Image {
  std::string sha256;
  int width = 0;
  int height = 0;
};

// A board to fill with Arc's objects.
struct Board {
  std::string uuid;
  raw_ptr<const ArcEasel> arc;
};

// One profile's boards: made, their images downloaded, then their objects
// saved. Owns itself until it reports.
class EaselImport {
 public:
  EaselImport(::Profile* profile,
              std::vector<ArcEaselRef> wanted,
              scoped_refptr<SharedArcEasels> arc_easels,
              base::OnceCallback<void(ArcEaselImportCounts)> callback)
      : wanted_(std::move(wanted)),
        easels_(std::move(arc_easels)),
        callback_(std::move(callback)) {
    if (easels::EaselStore* store = easels::EaselStore::Get(profile)) {
      store_ = store->GetWeakPtr();
      loader_factory_ = profile->GetDefaultStoragePartition()
                            ->GetURLLoaderFactoryForBrowserProcess();
    }
  }

  void Start() {
    if (!store_ || wanted_.empty()) {
      Finish();
      return;
    }
    store_->WhenLoaded(
        base::BindOnce(&EaselImport::OnLoaded, weak_factory_.GetWeakPtr()));
  }

 private:
  void OnLoaded() {
    easels::EaselStore* store = store_.get();
    if (!store) {
      Finish();
      return;
    }
    for (const ArcEaselRef& ref : wanted_) {
      const std::string uuid = ArcEaselUuid(ref.id).AsLowercaseString();
      if (uuid.empty()) {
        continue;
      }
      const auto found = easels_->data.find(ref.id);
      const ArcEasel* arc =
          found == easels_->data.end() ? nullptr : &found->second;
      if (store->FindEasel(uuid)) {
        // Imported before with its objects: left as the user has it.
        if (!store->ObjectsOf(uuid).empty()) {
          continue;
        }
      } else {
        std::string title = arc && !arc->title.empty()
                                ? arc->title
                                : base::UTF16ToUTF8(ref.title);
        if (title.empty()) {
          title = "Easel";
        }
        if (auto created = store->CreateEasel(std::move(title), uuid);
            !created.has_value()) {
          LOG(ERROR) << "Arc import: an Easel wasn't made: "
                     << created.error();
          continue;
        }
      }
      if (arc) {
        store->SetPalette(uuid, arc->palette);
      }
      if (!arc || !arc->has_objects) {
        ++counts_.missing;
        continue;
      }
      boards_.push_back({uuid, arc});
      for (const ArcEaselObject& object : arc->objects) {
        Queue(object.image_url);
        Queue(object.favicon_url);
      }
    }
    Pump();
  }

  void Queue(const GURL& url) {
    if (IsArcEaselImageUrl(url) && images_.emplace(url, std::nullopt).second) {
      queue_.push_back(url);
    }
  }

  // Starts downloads while there are free slots; saves once all are done.
  void Pump() {
    while (in_flight_ < kMaxDownloads && !queue_.empty()) {
      const GURL url = queue_.front();
      queue_.pop_front();
      Download(url);
    }
    if (in_flight_ == 0 && queue_.empty()) {
      SaveAll();
    }
  }

  void Download(const GURL& url) {
    ++in_flight_;
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    // Firebase serves the file itself; anything else isn't Arc's image.
    request->redirect_mode = network::mojom::RedirectMode::kError;
    std::unique_ptr<network::SimpleURLLoader> loader =
        network::SimpleURLLoader::Create(std::move(request),
                                         kTrafficAnnotation);
    loader->SetTimeoutDuration(kDownloadTimeout);
    network::SimpleURLLoader* raw = loader.get();
    loaders_[raw] = std::move(loader);
    raw->DownloadToString(
        loader_factory_.get(),
        base::BindOnce(&EaselImport::OnDownloaded, weak_factory_.GetWeakPtr(),
                       raw, url),
        kMaxImageBytes);
  }

  void OnDownloaded(network::SimpleURLLoader* loader,
                    GURL url,
                    std::optional<std::string> body) {
    const network::mojom::URLResponseHead* head = loader->ResponseInfo();
    const int status =
        head && head->headers ? head->headers->response_code() : 0;
    loaders_.erase(loader);
    if (!body || status != 200) {
      Done(url, std::nullopt);
      return;
    }
    easels::PrepareImage(
        std::move(*body),
        base::BindOnce(&EaselImport::OnPrepared, weak_factory_.GetWeakPtr(),
                       url));
  }

  void OnPrepared(GURL url,
                  base::expected<easels::PreparedImage, std::string> prepared) {
    std::optional<Image> image;
    if (prepared.has_value() && store_) {
      base::expected<std::string, std::string> sha =
          store_->AddAsset(std::move(prepared->data), prepared->mime_type);
      if (sha.has_value()) {
        image = Image{*sha, prepared->width, prepared->height};
      }
    }
    Done(url, std::move(image));
  }

  void Done(const GURL& url, std::optional<Image> image) {
    if (!image) {
      LOG(WARNING) << "Arc import: an Easel image couldn't be brought over";
    }
    images_[url] = std::move(image);
    --in_flight_;
    Pump();
  }

  const Image* ImageFor(const GURL& url) const {
    const auto it = images_.find(url);
    return it != images_.end() && it->second ? &*it->second : nullptr;
  }

  // |object| with its images, or as a link where they're missing.
  sync_pb::CompassEaselObject WithImages(const ArcEaselObject& object) const {
    const Image* image = ImageFor(object.image_url);
    sync_pb::CompassEaselObject result = object.object;
    if (result.has_image()) {
      if (!image) {
        return WithoutImages(object);
      }
      result.mutable_image()->set_asset(image->sha256);
      result.mutable_image()->set_pixel_width(image->width);
      result.mutable_image()->set_pixel_height(image->height);
    } else if (result.has_link()) {
      if (image) {
        result.mutable_link()->set_image_asset(image->sha256);
      }
    } else if (result.has_capture()) {
      if (!image) {
        return WithoutImages(object);
      }
      result.mutable_capture()->set_image_asset(image->sha256);
      if (const Image* favicon = ImageFor(object.favicon_url)) {
        result.mutable_capture()->set_favicon_asset(favicon->sha256);
      }
    }
    return result;
  }

  void SaveAll() {
    easels::EaselStore* store = store_.get();
    if (!store) {
      Finish();
      return;
    }
    for (const Board& board : boards_) {
      std::vector<sync_pb::CompassEaselObject> objects;
      for (const ArcEaselObject& arc_object : board.arc->objects) {
        sync_pb::CompassEaselObject object = WithImages(arc_object);
        object.set_uuid(ArcEaselObjectUuid(board.arc->id, arc_object.id)
                            .AsLowercaseString());
        object.set_easel_uuid(board.uuid);
        objects.push_back(std::move(object));
      }
      // All at once; if one is refused (a limit), the others one by one.
      if (!store->SaveObjects(objects).has_value()) {
        for (sync_pb::CompassEaselObject& object : objects) {
          if (auto saved = store->SaveObject(std::move(object));
              !saved.has_value()) {
            LOG(WARNING) << "Arc import: an Easel object was left out: "
                         << saved.error();
          }
        }
      }
      ++counts_.imported;
    }
    Finish();
  }

  void Finish() {
    base::OnceCallback<void(ArcEaselImportCounts)> callback =
        std::move(callback_);
    const ArcEaselImportCounts counts = counts_;
    delete this;
    std::move(callback).Run(counts);
  }

  const std::vector<ArcEaselRef> wanted_;
  const scoped_refptr<SharedArcEasels> easels_;
  base::OnceCallback<void(ArcEaselImportCounts)> callback_;
  base::WeakPtr<easels::EaselStore> store_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  std::vector<Board> boards_;
  // Every image URL wanted, and what became of it (nullopt: pending or
  // failed).
  std::map<GURL, std::optional<Image>> images_;
  std::deque<GURL> queue_;
  size_t in_flight_ = 0;
  std::map<network::SimpleURLLoader*,
           std::unique_ptr<network::SimpleURLLoader>>
      loaders_;
  ArcEaselImportCounts counts_;
  base::WeakPtrFactory<EaselImport> weak_factory_{this};
};

}  // namespace

void ImportArcEasels(::Profile* profile,
                     std::vector<ArcEaselRef> wanted,
                     scoped_refptr<SharedArcEasels> easels,
                     base::OnceCallback<void(ArcEaselImportCounts)> callback) {
  if (!easels) {
    easels = base::MakeRefCounted<SharedArcEasels>();
  }
  (new EaselImport(profile, std::move(wanted), std::move(easels),
                   std::move(callback)))
      ->Start();
}

}  // namespace compass::arc
