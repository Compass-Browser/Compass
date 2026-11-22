// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/filter_list_updater.h"

#include <algorithm>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/clock.h"
#include "compass/browser/adblock/abp_preprocessor.h"
#include "compass/browser/adblock/filter_lists.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace compass::adblock {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_adblock_filter_lists", R"(
        semantics {
          sender: "Compass ad blocker"
          description:
            "Compass's ad blocker downloads the filter lists it blocks ads, "
            "trackers and cookie banners with: EasyList, EasyPrivacy and the "
            "EasyList Cookie List, and the lists the user added by URL."
          trigger:
            "At most once a day for each list, shortly after launch and then "
            "every hour while Compass runs; when a list is added or turned "
            "on; and when the user presses Update now in the ad blocker's "
            "settings."
          data:
            "None: a plain GET of the list, with the ETag and Last-Modified "
            "date of the copy Compass has."
          destination: OTHER
          destination_other: "easylist.to, secure.fanboy.co.nz, and the "
                             "servers of the lists the user added."
        }
        policy {
          cookies_allowed: NO
          setting:
            "Turning the ad blocker off in Settings > Privacy stops the "
            "downloads."
          policy_exception_justification: "Not implemented."
        })");

// How much of a download is read to check it's a filter list and find its
// title.
constexpr size_t kHeaderBytes = 16 * 1024;

// The title, when the download at `downloaded` is a filter list and is now
// the list's copy at `destination`. Nothing when it isn't one (it's
// deleted).
std::optional<std::string> StoreDownload(const base::FilePath& downloaded,
                                         const base::FilePath& destination) {
  std::string start;
  start.resize(kHeaderBytes);
  base::File file(downloaded, base::File::FLAG_OPEN | base::File::FLAG_READ);
  std::optional<size_t> read =
      file.IsValid() ? file.ReadAtCurrentPos(base::as_writable_byte_span(start))
                     : std::nullopt;
  file.Close();
  start.resize(read.value_or(0));
  if (!LooksLikeFilterList(start) ||
      !base::CreateDirectory(destination.DirName()) ||
      !base::Move(downloaded, destination)) {
    base::DeleteFile(downloaded);
    return std::nullopt;
  }
  return FindListTitle(start);
}

}  // namespace

bool LooksLikeFilterList(std::string_view start) {
  std::string_view trimmed =
      base::TrimWhitespaceASCII(start, base::TRIM_LEADING);
  // A UTF-8 byte order mark.
  if (base::StartsWith(trimmed, "\xEF\xBB\xBF")) {
    trimmed.remove_prefix(3);
  }
  return start.contains("[Adblock") || base::StartsWith(trimmed, "!");
}

FilterListUpdater::FilterListUpdater(
    PrefService* local_state,
    const base::FilePath& user_data_dir,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    scoped_refptr<base::SequencedTaskRunner> file_task_runner,
    const base::Clock* clock,
    ListDoneCallback on_list_done)
    : local_state_(local_state),
      user_data_dir_(user_data_dir),
      url_loader_factory_(std::move(url_loader_factory)),
      file_task_runner_(std::move(file_task_runner)),
      clock_(clock),
      on_list_done_(std::move(on_list_done)) {}

FilterListUpdater::~FilterListUpdater() = default;

void FilterListUpdater::Update(bool force, base::OnceClosure on_done) {
  if (on_done) {
    on_done_.push_back(std::move(on_done));
  }
  const base::Time now = clock_->Now();
  for (const FilterList& list : GetEnabledFilterLists(local_state_)) {
    const FilterListState state = GetFilterListState(local_state_, list.id);
    const bool due = force || state.checked.is_null() ||
                     now - state.checked >= kMaxAge ||
                     state.checked > now;  // The clock went back.
    if (due && !std::ranges::contains(queue_, list.id) && current_ != list.id) {
      queue_.push_back(list.id);
    }
  }
  if (!current_) {
    StartNext();
  }
}

void FilterListUpdater::UpdateList(const std::string& id) {
  if (!std::ranges::contains(queue_, id) && current_ != id) {
    queue_.push_back(id);
  }
  if (!current_) {
    StartNext();
  }
}

void FilterListUpdater::StartNext() {
  current_.reset();
  while (!queue_.empty()) {
    std::string id = std::move(queue_.front());
    queue_.erase(queue_.begin());
    std::optional<FilterList> list = FindFilterList(local_state_, id);
    if (!list || !list->enabled) {
      continue;
    }
    current_ = id;
    // Asked only for a copy Compass still has: after it's gone (a cleared
    // folder) the server must send the list.
    file_task_runner_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&base::PathExists, GetListTextPath(user_data_dir_, id)),
        base::BindOnce(&FilterListUpdater::StartRequest,
                       weak_factory_.GetWeakPtr(), list->url));
    return;
  }
  for (base::OnceClosure& on_done : std::exchange(on_done_, {})) {
    std::move(on_done).Run();
  }
}

void FilterListUpdater::StartRequest(const std::string& url, bool has_copy) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE;
  const FilterListState state = GetFilterListState(local_state_, *current_);
  if (has_copy) {
    if (!state.etag.empty()) {
      request->headers.SetHeader(net::HttpRequestHeaders::kIfNoneMatch,
                                 state.etag);
    }
    if (!state.last_modified.empty()) {
      request->headers.SetHeader(net::HttpRequestHeaders::kIfModifiedSince,
                                 state.last_modified);
    }
  }
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
             network::SimpleURLLoader::RETRY_ON_5XX);
  loader_->DownloadToTempFile(
      url_loader_factory_.get(),
      base::BindOnce(&FilterListUpdater::OnDownloaded,
                     weak_factory_.GetWeakPtr()),
      kMaxListSize);
}

void FilterListUpdater::OnDownloaded(base::FilePath path) {
  const network::mojom::URLResponseHead* head = loader_->ResponseInfo();
  const int response_code =
      head && head->headers ? head->headers->response_code() : 0;
  const int net_error = loader_->NetError();
  FilterListState state = GetFilterListState(local_state_, *current_);

  if (path.empty()) {
    if (response_code == net::HTTP_NOT_MODIFIED) {
      state.checked = clock_->Now();
      state.error.clear();
    } else if (net_error == net::ERR_INSUFFICIENT_RESOURCES ||
               net_error == net::ERR_FILE_TOO_BIG) {
      state.error = "The list is too large";
    } else if (response_code >= 400) {
      state.error =
          base::StrCat({"The server answered ",
                        base::NumberToString(response_code)});
    } else {
      state.error = base::StrCat(
          {"Couldn't download the list (", net::ErrorToShortString(net_error),
           ")"});
    }
    loader_.reset();
    SetFilterListState(local_state_, *current_, state);
    Finish(/*changed=*/false);
    return;
  }

  // Kept for OnStored(): the headers are gone with the loader.
  pending_etag_.clear();
  pending_last_modified_.clear();
  if (head && head->headers) {
    pending_etag_ =
        std::string(head->headers->EnumerateHeader(nullptr, "ETag")
                        .value_or(std::string_view()));
    pending_last_modified_ =
        std::string(head->headers->EnumerateHeader(nullptr, "Last-Modified")
                        .value_or(std::string_view()));
  }
  loader_.reset();
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&StoreDownload, path,
                     GetListTextPath(user_data_dir_, *current_)),
      base::BindOnce(&FilterListUpdater::OnStored,
                     weak_factory_.GetWeakPtr()));
}

void FilterListUpdater::OnStored(std::optional<std::string> title) {
  FilterListState state = GetFilterListState(local_state_, *current_);
  const bool stored = title.has_value();
  if (stored) {
    const base::Time now = clock_->Now();
    state.etag = std::move(pending_etag_);
    state.last_modified = std::move(pending_last_modified_);
    state.checked = now;
    state.updated = now;
    state.error.clear();
    if (!title->empty()) {
      SetCustomFilterListTitle(local_state_, *current_, *title);
    }
  } else {
    state.error = "The download isn't a filter list";
  }
  SetFilterListState(local_state_, *current_, state);
  Finish(/*changed=*/stored);
}

void FilterListUpdater::Finish(bool changed) {
  const std::string id = *current_;
  current_.reset();
  if (on_list_done_) {
    on_list_done_.Run(id, changed);
  }
  StartNext();
}

}  // namespace compass::adblock
