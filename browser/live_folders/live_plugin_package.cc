// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_plugin_package.h"

#include <array>
#include <memory>
#include <optional>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "crypto/hash.h"
#include "net/base/url_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"

namespace compass {

namespace {

constexpr char kPluginsDirName[] = "Compass Plugins";
// Where the package came from, beside its files.
constexpr char kSourceFileName[] = "compass-source.json";
constexpr size_t kMaxManifestBytes = 256 * 1024;
constexpr size_t kMaxEntryBytes = 2 * 1024 * 1024;
constexpr size_t kMaxIconBytes = 256 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_live_plugin_install", R"(
        semantics {
          sender: "Compass live folders"
          description:
            "Downloads a live folder plugin (its manifest, script and icon) "
            "to install it."
          trigger: "The user installs a plugin from an address, or a synced "
                   "live folder needs a plugin this device doesn't have and "
                   "the user installs it."
          data: "None."
          destination: OTHER
          destination_other: "The address the plugin comes from."
        }
        policy {
          cookies_allowed: NO
          setting: "Only when the user installs a plugin."
          policy_exception_justification: "Not implemented."
        })");

void AppendLength(std::string& data, uint64_t length) {
  for (int i = 0; i < 8; ++i) {
    data.push_back(static_cast<char>((length >> (8 * i)) & 0xff));
  }
}

bool IsAllowedSource(const GURL& url) {
  return url.is_valid() &&
         (url.SchemeIs(url::kHttpsScheme) ||
          (url.SchemeIs(url::kHttpScheme) && net::IsLocalhost(url)));
}

// Fetches a manifest, then its entry and icon, one after another. Owns
// itself until it answers.
class PackageFetch {
 public:
  PackageFetch(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
               GURL manifest_url,
               LivePluginPackageCallback callback)
      : loader_factory_(std::move(loader_factory)),
        manifest_url_(std::move(manifest_url)),
        callback_(std::move(callback)) {}

  void Start() {
    Load(manifest_url_, kMaxManifestBytes,
         base::BindOnce(&PackageFetch::OnManifest, base::Unretained(this)));
  }

 private:
  using BodyCallback = base::OnceCallback<void(std::optional<std::string>)>;

  void Load(const GURL& url, size_t max, BodyCallback callback) {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                               kTrafficAnnotation);
    loader_->DownloadToString(
        loader_factory_.get(),
        base::BindOnce(
            [](PackageFetch* fetch, BodyCallback callback,
               std::optional<std::string> body) {
              const network::mojom::URLResponseHead* head =
                  fetch->loader_->ResponseInfo();
              const int status =
                  head && head->headers ? head->headers->response_code() : 0;
              // Redirects must stay where the manifest is.
              if (status != 200 ||
                  !url::Origin::Create(fetch->loader_->GetFinalURL())
                       .IsSameOriginWith(
                           url::Origin::Create(fetch->manifest_url_))) {
                body.reset();
              }
              std::move(callback).Run(std::move(body));
            },
            base::Unretained(this), std::move(callback)),
        max);
  }

  void OnManifest(std::optional<std::string> body) {
    if (!body) {
      return Fail("The plugin's manifest couldn't be downloaded.");
    }
    manifest_json_ = std::move(*body);
    std::optional<base::Value> raw =
        base::JSONReader::Read(manifest_json_, base::JSON_PARSE_RFC);
    auto manifest = raw ? ParseLivePluginManifest(*raw)
                        : base::unexpected(LivePluginErrors{"not JSON"});
    if (!manifest.has_value()) {
      return Fail("That isn't a live folder plugin's manifest: " +
                  manifest.error().front() + ".");
    }
    entry_name_ = manifest->entry;
    icon_name_ = manifest->icon;
    Load(manifest_url_.Resolve(entry_name_), kMaxEntryBytes,
         base::BindOnce(&PackageFetch::OnEntry, base::Unretained(this)));
  }

  void OnEntry(std::optional<std::string> body) {
    if (!body) {
      return Fail("The plugin's script couldn't be downloaded.");
    }
    entry_ = std::move(*body);
    if (icon_name_.empty()) {
      return Done();
    }
    Load(manifest_url_.Resolve(icon_name_), kMaxIconBytes,
         base::BindOnce(&PackageFetch::OnIcon, base::Unretained(this)));
  }

  void OnIcon(std::optional<std::string> body) {
    // A plugin without its icon still works.
    icon_ = body.value_or(std::string());
    Done();
  }

  void Done() {
    std::move(callback_).Run(MakeLivePluginPackage(
        manifest_url_, std::move(manifest_json_), std::move(entry_),
        std::move(icon_)));
    delete this;
  }

  void Fail(std::string message) {
    std::move(callback_).Run(base::unexpected(std::move(message)));
    delete this;
  }

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL manifest_url_;
  LivePluginPackageCallback callback_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string manifest_json_;
  std::string entry_name_;
  std::string icon_name_;
  std::string entry_;
  std::string icon_;
};

// "<id>" and "<version>" as directory names: what the manifest allows.
bool IsSafeName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '.' || c == '-' ||
                  c == '_';
         });
}

}  // namespace

LivePluginPackage::LivePluginPackage() = default;
LivePluginPackage::LivePluginPackage(const LivePluginPackage&) = default;
LivePluginPackage& LivePluginPackage::operator=(const LivePluginPackage&) =
    default;
LivePluginPackage::LivePluginPackage(LivePluginPackage&&) = default;
LivePluginPackage& LivePluginPackage::operator=(LivePluginPackage&&) = default;
LivePluginPackage::~LivePluginPackage() = default;

std::string LivePluginHash(const std::string& manifest_json,
                           const std::string& entry,
                           const std::string& icon) {
  std::string data;
  for (const std::string* part : {&manifest_json, &entry, &icon}) {
    AppendLength(data, part->size());
    data += *part;
  }
  return base::ToLowerASCII(
      base::HexEncode(crypto::hash::Sha256(data)));
}

base::expected<LivePluginPackage, std::string> MakeLivePluginPackage(
    const GURL& source,
    std::string manifest_json,
    std::string entry,
    std::string icon) {
  std::optional<base::Value> raw =
      base::JSONReader::Read(manifest_json, base::JSON_PARSE_RFC);
  if (!raw) {
    return base::unexpected("The plugin's manifest isn't JSON.");
  }
  auto manifest = ParseLivePluginManifest(*raw);
  if (!manifest.has_value()) {
    return base::unexpected("The plugin's manifest isn't valid: " +
                            manifest.error().front() + ".");
  }
  if (manifest->id.starts_with("compass.")) {
    return base::unexpected(
        "Plugin IDs starting with \"compass.\" are for built-in plugins.");
  }
  LivePluginPackage package;
  package.sha256 = LivePluginHash(manifest_json, entry, icon);
  package.manifest = std::move(*manifest);
  package.manifest_json = std::move(manifest_json);
  package.entry = std::move(entry);
  package.icon = std::move(icon);
  package.source = source;
  return package;
}

void FetchLivePluginPackage(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& manifest_url,
    LivePluginPackageCallback callback) {
  if (!IsAllowedSource(manifest_url)) {
    std::move(callback).Run(
        base::unexpected("Plugins install from an https:// address."));
    return;
  }
  (new PackageFetch(std::move(loader_factory), manifest_url,
                    std::move(callback)))
      ->Start();
}

base::FilePath LivePluginsDir(const base::FilePath& user_data_dir) {
  return user_data_dir.AppendASCII(kPluginsDirName);
}

bool WriteLivePluginPackage(const base::FilePath& plugins_dir,
                            const LivePluginPackage& package) {
  const LivePluginManifest& manifest = package.manifest;
  if (!IsSafeName(manifest.id) || !IsSafeName(manifest.version)) {
    return false;
  }
  const base::FilePath plugin_dir = plugins_dir.AppendASCII(manifest.id);
  const base::FilePath version_dir = plugin_dir.AppendASCII(manifest.version);
  // Other versions go: one installed version per plugin.
  base::DeletePathRecursively(plugin_dir);
  if (!base::CreateDirectory(version_dir)) {
    return false;
  }
  const std::string source =
      base::WriteJson(base::DictValue()
                          .Set("source", package.source.spec())
                          .Set("sha256", package.sha256))
          .value_or("{}");
  return base::WriteFile(version_dir.AppendASCII("manifest.json"),
                         package.manifest_json) &&
         base::WriteFile(version_dir.AppendASCII(manifest.entry),
                         package.entry) &&
         (manifest.icon.empty() || package.icon.empty() ||
          base::WriteFile(version_dir.AppendASCII(manifest.icon),
                          package.icon)) &&
         base::WriteFile(version_dir.AppendASCII(kSourceFileName), source);
}

std::vector<LivePluginPackage> ReadLivePluginPackages(
    const base::FilePath& plugins_dir) {
  std::vector<LivePluginPackage> packages;
  base::FileEnumerator plugins(plugins_dir, false,
                               base::FileEnumerator::DIRECTORIES);
  for (base::FilePath plugin = plugins.Next(); !plugin.empty();
       plugin = plugins.Next()) {
    base::FileEnumerator versions(plugin, false,
                                  base::FileEnumerator::DIRECTORIES);
    for (base::FilePath dir = versions.Next(); !dir.empty();
         dir = versions.Next()) {
      std::string manifest_json;
      std::string source_json;
      if (!base::ReadFileToString(dir.AppendASCII("manifest.json"),
                                  &manifest_json) ||
          !base::ReadFileToString(dir.AppendASCII(kSourceFileName),
                                  &source_json)) {
        continue;
      }
      std::optional<base::Value> raw =
          base::JSONReader::Read(manifest_json, base::JSON_PARSE_RFC);
      auto manifest = raw ? ParseLivePluginManifest(*raw)
                          : base::unexpected(LivePluginErrors{});
      if (!manifest.has_value() ||
          dir.BaseName().MaybeAsASCII() != manifest->version ||
          plugin.BaseName().MaybeAsASCII() != manifest->id) {
        continue;
      }
      std::string entry;
      std::string icon;
      if (!base::ReadFileToString(dir.AppendASCII(manifest->entry), &entry)) {
        continue;
      }
      if (!manifest->icon.empty()) {
        base::ReadFileToString(dir.AppendASCII(manifest->icon), &icon);
      }
      std::optional<base::Value> source =
          base::JSONReader::Read(source_json, base::JSON_PARSE_RFC);
      const std::string* url =
          source && source->is_dict() ? source->GetDict().FindString("source")
                                      : nullptr;
      auto package = MakeLivePluginPackage(url ? GURL(*url) : GURL(),
                                           std::move(manifest_json),
                                           std::move(entry), std::move(icon));
      if (package.has_value()) {
        packages.push_back(std::move(*package));
      }
    }
  }
  return packages;
}

bool DeleteLivePluginPackage(const base::FilePath& plugins_dir,
                             const std::string& id) {
  return IsSafeName(id) &&
         base::DeletePathRecursively(plugins_dir.AppendASCII(id));
}

base::FilePath LivePluginFilePath(const base::FilePath& plugins_dir,
                                  std::string_view relative) {
  std::vector<std::string_view> parts = base::SplitStringPiece(
      relative, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() != 3 || !std::ranges::all_of(parts, IsSafeName)) {
    return base::FilePath();
  }
  return plugins_dir.AppendASCII(parts[0])
      .AppendASCII(parts[1])
      .AppendASCII(parts[2]);
}

}  // namespace compass
