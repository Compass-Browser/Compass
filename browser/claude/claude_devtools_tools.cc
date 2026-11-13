// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Claude's developer tools (docs/design/claude.md, "Developer tools"): the
// network, styles, storage, performance and emulation of the tab Claude acts
// on, through the same DevTools session as the page tools. They need "Let
// Claude use developer tools" (ClaudeTools::CheckDevTools()).

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "compass/browser/claude/claude_tools.h"
#include "compass/browser/claude/devtools_format.h"
#include "compass/browser/claude/devtools_session.h"
#include "content/public/browser/web_contents.h"
#include "third_party/re2/src/re2/re2.h"

namespace compass::claude {

namespace fmt = devtools_format;

namespace {

// Requests kept: the most recent ones.
constexpr size_t kNetworkSize = 500;
// A URL in a list is cut to this many bytes (data: URLs are long).
constexpr size_t kMaxListedUrl = 300;
// A body is given in parts of this many bytes.
constexpr size_t kMaxBodyPart = 20000;
// Bodies are masked whole, up to this size; past it only the first part.
constexpr size_t kMaxMaskedBody = 2 * 1024 * 1024;
// A request's own body shown in network_response.
constexpr size_t kMaxPostData = 4000;

std::string Str(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

base::DictValue NetworkEnableParams() {
  return base::DictValue()
      .Set("maxTotalBufferSize", 50 * 1024 * 1024)
      .Set("maxResourceBufferSize", 10 * 1024 * 1024)
      .Set("maxPostDataSize", 64 * 1024);
}

// "r12" → 12; 0 if it isn't one.
uint64_t ParseRequestId(const std::string& id) {
  uint64_t seq = 0;
  if (id.size() < 2 || (id[0] != 'r' && id[0] != 'R') ||
      !base::StringToUint64(std::string_view(id).substr(1), &seq)) {
    return 0;
  }
  return seq;
}

}  // namespace

ClaudeTools::NetworkEntry::NetworkEntry() = default;
ClaudeTools::NetworkEntry::NetworkEntry(NetworkEntry&&) = default;
ClaudeTools::NetworkEntry& ClaudeTools::NetworkEntry::operator=(
    NetworkEntry&&) = default;
ClaudeTools::NetworkEntry::~NetworkEntry() = default;

void ClaudeTools::EnsureNetwork(base::OnceClosure next) {
  if (network_enabled_) {
    std::move(next).Run();
    return;
  }
  network_enabled_ = true;
  devtools().Send(
      "Network.enable", NetworkEnableParams(),
      base::BindOnce(
          [](base::WeakPtr<ClaudeTools> self, base::OnceClosure next,
             Result result) {
            if (!self) {
              return;
            }
            if (!result.has_value()) {
              self->network_enabled_ = false;
            }
            std::move(next).Run();
          },
          weak_factory_.GetWeakPtr(), std::move(next)));
}

ClaudeTools::NetworkEntry* ClaudeTools::FindNetworkEntry(
    const std::string& request_id) {
  for (auto it = network_.rbegin(); it != network_.rend(); ++it) {
    if (it->request_id == request_id) {
      return &*it;
    }
  }
  return nullptr;
}

void ClaudeTools::OnNetworkEvent(const std::string& method,
                                 const base::DictValue& params) {
  const std::string request_id = Str(params, "requestId");
  if (method == "Network.requestWillBeSent") {
    const double timestamp = params.FindDouble("timestamp").value_or(0);
    if (const base::DictValue* redirect = params.FindDict("redirectResponse")) {
      // The same id goes on to the next URL: the redirect ends here.
      if (NetworkEntry* entry = FindNetworkEntry(request_id)) {
        entry->status = redirect->FindInt("status").value_or(0);
        entry->status_text = Str(*redirect, "statusText");
        entry->mime_type = Str(*redirect, "mimeType");
        if (const base::DictValue* headers = redirect->FindDict("headers")) {
          entry->response_headers = headers->Clone();
        }
        entry->finished = true;
        entry->end = timestamp;
      }
    }
    const base::DictValue* request = params.FindDict("request");
    if (!request) {
      return;
    }
    if (network_.size() == kNetworkSize) {
      network_.pop_front();
    }
    NetworkEntry entry;
    entry.seq = next_network_seq_++;
    entry.request_id = request_id;
    entry.method = Str(*request, "method");
    entry.url = Str(*request, "url");
    entry.type = Str(params, "type");
    entry.start = timestamp;
    if (const base::DictValue* headers = request->FindDict("headers")) {
      entry.request_headers = headers->Clone();
    }
    entry.post_data = Str(*request, "postData");
    entry.has_post_data =
        request->FindBool("hasPostData").value_or(!entry.post_data.empty());
    network_.push_back(std::move(entry));
    return;
  }
  if (method == "Network.webSocketCreated") {
    if (network_.size() == kNetworkSize) {
      network_.pop_front();
    }
    NetworkEntry entry;
    entry.seq = next_network_seq_++;
    entry.request_id = request_id;
    entry.method = "GET";
    entry.url = Str(params, "url");
    entry.type = "WebSocket";
    network_.push_back(std::move(entry));
    return;
  }
  NetworkEntry* entry = FindNetworkEntry(request_id);
  if (!entry) {
    return;
  }
  if (method == "Network.responseReceived") {
    if (const std::string* type = params.FindString("type")) {
      entry->type = *type;
    }
    const base::DictValue* response = params.FindDict("response");
    if (!response) {
      return;
    }
    entry->status = response->FindInt("status").value_or(0);
    entry->status_text = Str(*response, "statusText");
    entry->mime_type = Str(*response, "mimeType");
    if (const base::DictValue* headers = response->FindDict("headers")) {
      entry->response_headers = headers->Clone();
    }
    if (response->FindBool("fromDiskCache").value_or(false) ||
        response->FindBool("fromPrefetchCache").value_or(false)) {
      entry->from_cache = true;
    }
  } else if (method == "Network.requestServedFromCache") {
    entry->from_cache = true;
  } else if (method == "Network.loadingFinished") {
    entry->finished = true;
    entry->end = params.FindDouble("timestamp").value_or(entry->start);
    entry->size = params.FindDouble("encodedDataLength").value_or(0);
  } else if (method == "Network.loadingFailed") {
    entry->finished = true;
    entry->end = params.FindDouble("timestamp").value_or(entry->start);
    entry->error = Str(params, "errorText");
    if (params.FindBool("canceled").value_or(false)) {
      entry->error += " (canceled)";
    }
    if (const std::string* blocked = params.FindString("blockedReason")) {
      base::StrAppend(&entry->error, {" (blocked: ", *blocked, ")"});
    }
  }
}

void ClaudeTools::NetworkRequests(base::DictValue input) {
  const bool only_new = input.FindBool("only_new").value_or(false);
  const bool failed = input.FindBool("failed").value_or(false);
  const int limit = std::clamp(input.FindInt("limit").value_or(50), 1, 200);
  std::vector<std::string> types;
  if (const base::ListValue* list = input.FindList("types")) {
    for (const base::Value& type : *list) {
      if (type.is_string()) {
        types.push_back(base::ToLowerASCII(type.GetString()));
      }
    }
  }
  std::unique_ptr<re2::RE2> pattern;
  if (const std::string* text = input.FindString("url")) {
    pattern = std::make_unique<re2::RE2>(*text, re2::RE2::Quiet);
    if (!pattern->ok()) {
      Finish(ToolResult::Error(
          base::StrCat({"Bad pattern: ", pattern->error()})));
      return;
    }
  }
  EnsureNetwork(base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self, bool only_new, bool failed,
         int limit, std::vector<std::string> types,
         std::unique_ptr<re2::RE2> pattern) {
        if (!self) {
          return;
        }
        if (!self->network_enabled_) {
          self->Finish(ToolResult::Error(
              "Claude can't record this tab's requests right now."));
          return;
        }
        std::vector<const NetworkEntry*> matching;
        for (const NetworkEntry& entry : self->network_) {
          if (only_new && entry.seq <= self->network_read_seq_) {
            continue;
          }
          if (!types.empty() &&
              std::ranges::find(types, base::ToLowerASCII(entry.type)) ==
                  types.end()) {
            continue;
          }
          if (failed && entry.error.empty() && entry.status < 400) {
            continue;
          }
          if (pattern && !re2::RE2::PartialMatch(entry.url, *pattern)) {
            continue;
          }
          matching.push_back(&entry);
        }
        if (!self->network_.empty()) {
          self->network_read_seq_ = self->network_.back().seq;
        }
        if (matching.empty()) {
          self->Finish(ToolResult::Text(
              only_new ? "No new requests."
                       : "No requests recorded. Requests are recorded from "
                         "when you first used this tab in the chat; "
                         "navigate with \"reload\" to see the page load."));
          return;
        }
        std::string text;
        const size_t skipped =
            matching.size() > static_cast<size_t>(limit)
                ? matching.size() - limit
                : 0;
        if (skipped) {
          base::StrAppend(&text, {base::NumberToString(skipped),
                                  " older requests not shown.\n"});
        }
        for (size_t i = skipped; i < matching.size(); ++i) {
          const NetworkEntry& entry = *matching[i];
          std::string status;
          if (!entry.error.empty()) {
            status = "failed";
          } else if (entry.status) {
            status = base::NumberToString(entry.status);
          } else {
            status = "…";
          }
          const std::string url = fmt::Cut(
              fmt::MaskSecretsInText(entry.url),
              entry.url.starts_with("data:") ? 100 : kMaxListedUrl);
          base::StrAppend(
              &text, {"r", base::NumberToString(entry.seq), " ",
                      entry.method, " ", status, " ",
                      entry.type.empty() ? std::string("Other") : entry.type,
                      " ", url, " — "});
          if (!entry.error.empty()) {
            text += entry.error;
          } else if (!entry.finished) {
            text += "pending";
          } else {
            std::vector<std::string> details;
            if (!entry.mime_type.empty()) {
              details.push_back(entry.mime_type);
            }
            details.push_back(entry.from_cache ? std::string("cache")
                                               : fmt::FormatBytes(entry.size));
            if (entry.end > entry.start && entry.start > 0) {
              details.push_back(fmt::FormatDuration(entry.end - entry.start));
            }
            text += base::JoinString(details, ", ");
          }
          text += "\n";
        }
        self->Finish(ToolResult::Text(std::move(text)));
      },
      run_weak_factory_.GetWeakPtr(), only_new, failed, limit,
      std::move(types), std::move(pattern)));
}

void ClaudeTools::NetworkResponse(base::DictValue input) {
  const uint64_t seq = ParseRequestId(Str(input, "id"));
  const NetworkEntry* entry = nullptr;
  for (const NetworkEntry& candidate : network_) {
    if (candidate.seq == seq) {
      entry = &candidate;
    }
  }
  if (!entry) {
    Finish(ToolResult::Error(
        "No request has that id: give one from network_requests (like "
        "\"r12\"). Older ones are dropped after 500."));
    return;
  }
  const bool show_secrets = input.FindBool("show_secrets").value_or(false);
  const bool want_body = input.FindBool("body").value_or(true);
  const size_t offset =
      static_cast<size_t>(std::max(0, input.FindInt("offset").value_or(0)));

  std::string text = base::StrCat(
      {entry->method, " ",
       show_secrets ? entry->url : fmt::MaskSecretsInText(entry->url), "\n"});
  if (!entry->error.empty()) {
    base::StrAppend(&text, {"Failed: ", entry->error, "\n"});
  } else if (entry->status) {
    base::StrAppend(&text, {"Status: ", base::NumberToString(entry->status),
                            " ", entry->status_text, "\n"});
  } else {
    text += "Status: pending\n";
  }
  if (!entry->type.empty()) {
    base::StrAppend(&text, {"Type: ", entry->type, "\n"});
  }
  base::StrAppend(&text, {"\nRequest headers:\n",
                          fmt::FormatHeaders(entry->request_headers,
                                             show_secrets)});
  if (!entry->post_data.empty()) {
    const std::string post = show_secrets
                                 ? entry->post_data
                                 : fmt::MaskSecretsInText(entry->post_data);
    base::StrAppend(&text,
                    {"\nRequest body:\n", fmt::Cut(post, kMaxPostData), "\n"});
  } else if (entry->has_post_data) {
    text += "\nRequest body: too large to show.\n";
  }
  if (!entry->response_headers.empty()) {
    base::StrAppend(&text, {"\nResponse headers:\n",
                            fmt::FormatHeaders(entry->response_headers,
                                               show_secrets)});
  }
  if (!want_body || !entry->error.empty() || entry->type == "WebSocket") {
    Finish(ToolResult::Text(std::move(text)));
    return;
  }
  if (!entry->finished) {
    text += "\nThe body is still loading.";
    Finish(ToolResult::Text(std::move(text)));
    return;
  }
  if (entry->status >= 300 && entry->status < 400) {
    text += "\nA redirect: no body.";
    Finish(ToolResult::Text(std::move(text)));
    return;
  }
  const std::string mime_type = entry->mime_type;
  EnsureNetwork(base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self, std::string request_id,
         std::string mime_type, std::string text, bool show_secrets,
         size_t offset) {
        if (!self) {
          return;
        }
        self->Send(
            "Network.getResponseBody",
            base::DictValue().Set("requestId", request_id),
            base::BindOnce(
                [](base::WeakPtr<ClaudeTools> self, std::string mime_type,
                   std::string text, bool show_secrets, size_t offset,
                   Result body) {
                  if (!self) {
                    return;
                  }
                  if (!body.has_value()) {
                    text +=
                        "\nThe body isn't available anymore (the page went "
                        "on, or Claude let go of the tab since). Load it "
                        "again, then ask for it.";
                    self->Finish(ToolResult::Text(std::move(text)));
                    return;
                  }
                  std::string data = Str(*body, "body");
                  if (body->FindBool("base64Encoded").value_or(false)) {
                    std::string decoded;
                    if (!fmt::IsTextMimeType(mime_type) ||
                        !base::Base64Decode(data, &decoded) ||
                        !base::IsStringUTF8(decoded)) {
                      base::StrAppend(
                          &text,
                          {"\nA binary body (", mime_type.empty()
                                                    ? std::string("unknown")
                                                    : mime_type,
                           "), not shown."});
                      self->Finish(ToolResult::Text(std::move(text)));
                      return;
                    }
                    data = std::move(decoded);
                  }
                  if (data.empty()) {
                    text += "\nAn empty body.";
                    self->Finish(ToolResult::Text(std::move(text)));
                    return;
                  }
                  if (!show_secrets) {
                    data = data.size() <= kMaxMaskedBody
                               ? fmt::MaskSecretsInText(data)
                               : fmt::MaskSecretsInText(
                                     std::string_view(data).substr(
                                         0, offset + kMaxBodyPart));
                  }
                  size_t next = 0;
                  const std::string part =
                      fmt::Part(data, offset, kMaxBodyPart, &next);
                  base::StrAppend(
                      &text,
                      {"\nResponse body (", fmt::FormatBytes(data.size()),
                       offset ? base::StrCat({", from ",
                                              base::NumberToString(offset)})
                              : std::string(),
                       "):\n", part});
                  if (next) {
                    base::StrAppend(&text,
                                    {"\n… more: offset ",
                                     base::NumberToString(next)});
                  }
                  self->Finish(ToolResult::Text(std::move(text)));
                },
                self->run_weak_factory_.GetWeakPtr(), std::move(mime_type),
                std::move(text), show_secrets, offset));
      },
      run_weak_factory_.GetWeakPtr(), entry->request_id, mime_type,
      std::move(text), show_secrets, offset));
}

}  // namespace compass::claude
