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
#include "base/i18n/time_formatting.h"
#include "base/time/time.h"
#include "content/public/browser/web_contents.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"
#include "url/origin.h"

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

// inspect_element's computed styles by default: layout, box, flex and grid,
// color and font.
constexpr const char* kDefaultComputed[] = {
    "display", "position", "top", "right", "bottom", "left", "z-index",
    "width", "height", "min-width", "max-width", "min-height", "max-height",
    "box-sizing", "margin-top", "margin-right", "margin-bottom",
    "margin-left", "padding-top", "padding-right", "padding-bottom",
    "padding-left", "border-top-width", "border-right-width",
    "border-bottom-width", "border-left-width", "overflow-x", "overflow-y",
    "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis",
    "justify-content", "align-items", "align-self", "gap",
    "grid-template-columns", "grid-template-rows", "color",
    "background-color", "background-image", "font-family", "font-size",
    "font-weight", "line-height", "text-align", "white-space", "opacity",
    "visibility", "transform", "pointer-events", "cursor",
};
// Rules inspect_element shows, and its whole answer's size.
constexpr size_t kMaxRules = 20;
constexpr size_t kMaxInspectBytes = 15000;
// page_storage: items shown per store, and a value's length.
constexpr size_t kMaxStorageItems = 200;
constexpr size_t kMaxStorageValue = 300;
constexpr size_t kMaxShownStorageValue = 2000;
// What an approval shows of a value.
constexpr size_t kMaxApprovalValue = 2000;
constexpr char kInspectGroup[] = "compass-inspect";

std::string OneLine(std::string_view text) {
  return base::CollapseWhitespaceASCII(text, true);
}

// "<div id="box" class="card">" from DOM.describeNode's node.
std::string DescribeNode(const base::DictValue& node) {
  std::string text = base::StrCat({"<", Str(node, "localName")});
  if (const base::ListValue* attributes = node.FindList("attributes")) {
    for (size_t i = 0; i + 1 < attributes->size(); i += 2) {
      const base::Value& name = (*attributes)[i];
      const base::Value& value = (*attributes)[i + 1];
      if (!name.is_string() || !value.is_string()) {
        continue;
      }
      static constexpr std::string_view kShown[] = {
          "id", "class", "name", "type", "role", "href", "src", "aria-label"};
      if (!std::ranges::contains(kShown, std::string_view(name.GetString()))) {
        continue;
      }
      base::StrAppend(&text, {" ", name.GetString(), "=\"",
                              fmt::Cut(value.GetString(), 80), "\""});
    }
  }
  return text + ">";
}

// A quad's [x1, y1, …, x4, y4] at |index|.
double QuadAt(const base::ListValue* quad, size_t index) {
  if (!quad || quad->size() < 8 ||
      !((*quad)[index].is_double() || (*quad)[index].is_int())) {
    return 0;
  }
  return (*quad)[index].GetDouble();
}

// "10 10 10 10": the edges between an outer and an inner quad.
std::string Edges(const base::ListValue* outer, const base::ListValue* inner) {
  return base::StringPrintf(
      "%.0f %.0f %.0f %.0f", QuadAt(inner, 1) - QuadAt(outer, 1),
      QuadAt(outer, 2) - QuadAt(inner, 2), QuadAt(outer, 5) - QuadAt(inner, 5),
      QuadAt(inner, 0) - QuadAt(outer, 0));
}

std::string FormatBox(const base::DictValue& model) {
  const base::ListValue* content = model.FindList("content");
  const base::ListValue* padding = model.FindList("padding");
  const base::ListValue* border = model.FindList("border");
  const base::ListValue* margin = model.FindList("margin");
  return base::StringPrintf(
      "Box: %d×%d at %.0f,%.0f; content %.0f×%.0f; padding %s; border %s; "
      "margin %s (top right bottom left)",
      model.FindInt("width").value_or(0), model.FindInt("height").value_or(0),
      QuadAt(border, 0), QuadAt(border, 1),
      QuadAt(content, 2) - QuadAt(content, 0),
      QuadAt(content, 5) - QuadAt(content, 1),
      Edges(padding, content).c_str(), Edges(border, padding).c_str(),
      Edges(margin, border).c_str());
}

// A rule's declarations, a line each: the ones written in the rule (not the
// longhands a shorthand implies), without disabled ones.
std::string Declarations(const base::DictValue& style) {
  const base::ListValue* properties = style.FindList("cssProperties");
  if (!properties) {
    return std::string();
  }
  bool any_written = false;
  for (const base::Value& property : *properties) {
    if (property.is_dict() && property.GetDict().FindString("text")) {
      any_written = true;
    }
  }
  std::string text;
  for (const base::Value& property : *properties) {
    if (!property.is_dict()) {
      continue;
    }
    const base::DictValue& dict = property.GetDict();
    if (dict.FindBool("disabled").value_or(false) ||
        dict.FindBool("implicit").value_or(false) ||
        (any_written && !dict.FindString("text"))) {
      continue;
    }
    base::StrAppend(&text, {"    ", Str(dict, "name"), ": ",
                            fmt::Cut(Str(dict, "value"), 200),
                            dict.FindBool("important").value_or(false)
                                ? " !important"
                                : "",
                            ";\n"});
  }
  return text;
}

std::string FormatCookie(const base::DictValue& cookie, bool show_values) {
  const std::string value = Str(cookie, "value");
  std::string text = base::StrCat(
      {"  ", Str(cookie, "name"), "=",
       show_values ? fmt::Cut(value, kMaxShownStorageValue) : fmt::Hidden(value),
       "; domain ", Str(cookie, "domain"), "; path ", Str(cookie, "path")});
  const double expires = cookie.FindDouble("expires").value_or(-1);
  if (cookie.FindBool("session").value_or(false) || expires <= 0) {
    text += "; session";
  } else {
    base::StrAppend(
        &text,
        {"; expires ",
         base::UnlocalizedTimeFormatWithPattern(
             base::Time::FromSecondsSinceUnixEpoch(expires), "yyyy-MM-dd")});
  }
  if (cookie.FindBool("httpOnly").value_or(false)) {
    text += "; HttpOnly";
  }
  if (cookie.FindBool("secure").value_or(false)) {
    text += "; Secure";
  }
  if (const std::string* same_site = cookie.FindString("sameSite")) {
    base::StrAppend(&text, {"; SameSite=", *same_site});
  }
  return text + "\n";
}

// The page's Core Web Vitals and load timings, from its own performance
// timeline (buffered observers), in milliseconds.
constexpr char kVitalsScript[] = R"js((async () => {
  const take = (type, extra) => new Promise((resolve) => {
    const entries = [];
    try {
      const observer = new PerformanceObserver(
          (list) => entries.push(...list.getEntries()));
      observer.observe({type, buffered: true, ...extra});
      setTimeout(() => { observer.disconnect(); resolve(entries); }, 100);
    } catch (e) {
      resolve(entries);
    }
  });
  const [lcp, shifts, events, frames] = await Promise.all([
    take('largest-contentful-paint'), take('layout-shift'),
    take('event', {durationThreshold: 16}), take('long-animation-frame')]);
  const nav = performance.getEntriesByType('navigation')[0];
  const fcp = performance.getEntriesByName('first-contentful-paint')[0];
  let cls = 0, session = 0, first = 0, last = 0;
  for (const shift of shifts) {
    if (shift.hadRecentInput) continue;
    if (session && shift.startTime - last < 1000 &&
        shift.startTime - first < 5000) {
      session += shift.value;
    } else {
      session = shift.value;
      first = shift.startTime;
    }
    last = shift.startTime;
    cls = Math.max(cls, session);
  }
  const interactions = events.filter((e) => e.interactionId);
  const largest = lcp[lcp.length - 1];
  const name = (el) => el ?
      el.localName + (el.id ? '#' + el.id : '') : '';
  const resources = performance.getEntriesByType('resource').map((r) => ({
    url: r.name, type: r.initiatorType, duration: r.duration,
    size: r.transferSize || 0}));
  return {
    url: location.href,
    ttfb: nav ? nav.responseStart : -1,
    fcp: fcp ? fcp.startTime : -1,
    dcl: nav ? nav.domContentLoadedEventEnd : -1,
    load: nav ? nav.loadEventEnd : -1,
    lcp: largest ? largest.startTime : -1,
    lcpElement: largest ? name(largest.element) : '',
    cls,
    inp: interactions.reduce((m, e) => Math.max(m, e.duration), -1),
    interactions: interactions.length,
    longFrames: frames.length,
    blocking: frames.reduce((t, f) => t + (f.blockingDuration || 0), 0),
    resources: resources.length,
    transfer: resources.reduce((t, r) => t + r.size, 0),
    slowest: resources.sort((a, b) => b.duration - a.duration).slice(0, 5),
  };
})())js";

// "good", "needs improvement" or "poor", by web.dev's thresholds.
std::string Rate(double value, double good, double poor) {
  return value <= good ? "good" : value <= poor ? "needs improvement" : "poor";
}

// A timing in milliseconds, or "n/a" for -1.
std::string Ms(double ms) {
  return ms < 0 ? std::string("n/a") : fmt::FormatDuration(ms / 1000);
}

double Metric(const base::ListValue* metrics, std::string_view name) {
  if (!metrics) {
    return 0;
  }
  for (const base::Value& metric : *metrics) {
    if (metric.is_dict() && Str(metric.GetDict(), "name") == name) {
      return metric.GetDict().FindDouble("value").value_or(0);
    }
  }
  return 0;
}

struct DevicePreset {
  const char* name;
  const char* label;
  int width;
  int height;
  double scale;
  bool mobile;
  const char* user_agent;
};

constexpr DevicePreset kDevices[] = {
    {"iphone", "iPhone", 393, 852, 3, true,
     "Mozilla/5.0 (iPhone; CPU iPhone OS 19_0 like Mac OS X) "
     "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/19.0 Mobile/15E148 "
     "Safari/604.1"},
    {"android", "Android phone", 412, 915, 2.625, true,
     "Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, "
     "like Gecko) Chrome/157.0.0.0 Mobile Safari/537.36"},
    {"ipad", "iPad", 820, 1180, 2, true,
     "Mozilla/5.0 (iPad; CPU OS 19_0 like Mac OS X) AppleWebKit/605.1.15 "
     "(KHTML, like Gecko) Version/19.0 Mobile/15E148 Safari/604.1"},
    {"laptop", "laptop", 1366, 768, 1, false, ""},
};

struct NetworkPreset {
  const char* name;
  bool offline;
  double latency_ms;
  // Bytes a second; -1 for no limit.
  double download;
  double upload;
};

constexpr NetworkPreset kNetworks[] = {
    {"offline", true, 0, 0, 0},
    {"slow_3g", false, 2000, 50 * 1000, 50 * 1000},
    {"fast_3g", false, 563, 180 * 1000, 84 * 1000},
    {"slow_4g", false, 150, 1600 * 1000, 750 * 1000},
    {"none", false, 0, -1, -1},
};

bool Matches(const std::string& name, const std::string& filter) {
  return filter.empty() || base::ToLowerASCII(name).find(filter) !=
                               std::string::npos;
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


// ---- inspect_element ------------------------------------------------------

struct ClaudeTools::InspectState {
  std::string selector;
  std::vector<std::string> properties;
  bool user_agent = false;
  int node_id = 0;
  std::string node;
  base::ListValue computed;
  base::DictValue matched;
  std::string box;
  base::ListValue listeners;
};

void ClaudeTools::OnCssEvent(const std::string& method,
                             const base::DictValue& params) {
  if (method == "CSS.styleSheetAdded") {
    const base::DictValue* header = params.FindDict("header");
    if (!header) {
      return;
    }
    StyleSheet sheet;
    sheet.url = Str(*header, "sourceURL");
    sheet.start_line =
        static_cast<int>(header->FindDouble("startLine").value_or(0));
    sheet.is_inline = header->FindBool("isInline").value_or(false);
    style_sheets_[Str(*header, "styleSheetId")] = std::move(sheet);
  } else if (method == "CSS.styleSheetRemoved") {
    style_sheets_.erase(Str(params, "styleSheetId"));
  }
}

void ClaudeTools::InspectElement(base::DictValue input) {
  auto state = std::make_unique<InspectState>();
  state->user_agent = input.FindBool("user_agent").value_or(false);
  if (const base::ListValue* properties = input.FindList("properties")) {
    for (const base::Value& property : *properties) {
      if (property.is_string()) {
        state->properties.push_back(base::ToLowerASCII(property.GetString()));
      }
    }
  }
  if (const std::string* selector = input.FindString("selector");
      selector && !selector->empty()) {
    state->selector = *selector;
    InspectStep(std::move(state), 0, base::DictValue());
    return;
  }
  const std::string* ref = input.FindString("ref");
  if (!ref) {
    Finish(ToolResult::Error("Give a ref from read_page or find, or a CSS "
                             "selector."));
    return;
  }
  Agent("selector", base::DictValue().Set("ref", *ref),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self,
               std::unique_ptr<InspectState> state, Result found) {
              if (!self) {
                return;
              }
              if (!found.has_value()) {
                self->Finish(ToolResult::Error(found.error()));
                return;
              }
              state->selector = Str(*found, "selector");
              self->InspectStep(std::move(state), 0, base::DictValue());
            },
            run_weak_factory_.GetWeakPtr(), std::move(state)));
}

void ClaudeTools::InspectStep(std::unique_ptr<InspectState> state,
                              int step,
                              Result result) {
  // Steps 1 to 6 must work; the box, the listeners may not (an element
  // that isn't rendered has no box).
  if (!result.has_value() && step <= 6) {
    Finish(ToolResult::Error(result.error()));
    return;
  }
  auto next = [&](const char* method, base::DictValue params) {
    Send(method, std::move(params),
         base::BindOnce(&ClaudeTools::InspectStep,
                        run_weak_factory_.GetWeakPtr(), std::move(state),
                        step + 1));
  };
  switch (step) {
    case 0:
      next("DOM.getDocument", base::DictValue().Set("depth", 0));
      return;
    case 1: {
      const int root = result->FindIntByDottedPath("root.nodeId").value_or(0);
      const std::string selector = state->selector;
      next("DOM.querySelector",
           base::DictValue().Set("nodeId", root).Set("selector", selector));
      return;
    }
    case 2:
      state->node_id = result->FindInt("nodeId").value_or(0);
      if (!state->node_id) {
        Finish(ToolResult::Error(base::StrCat(
            {"No element matches ", state->selector, "."})));
        return;
      }
      next("DOM.describeNode", base::DictValue().Set("nodeId", state->node_id));
      return;
    case 3:
      if (const base::DictValue* node = result->FindDict("node")) {
        state->node = DescribeNode(*node);
      }
      if (css_enabled_) {
        InspectStep(std::move(state), 4, base::DictValue());
        return;
      }
      css_enabled_ = true;
      next("CSS.enable", base::DictValue());
      return;
    case 4: {
      const int node_id = state->node_id;
      next("CSS.getComputedStyleForNode",
           base::DictValue().Set("nodeId", node_id));
      return;
    }
    case 5: {
      if (base::ListValue* computed = result->FindList("computedStyle")) {
        state->computed = std::move(*computed);
      }
      const int node_id = state->node_id;
      next("CSS.getMatchedStylesForNode",
           base::DictValue().Set("nodeId", node_id));
      return;
    }
    case 6: {
      state->matched = std::move(*result);
      const int node_id = state->node_id;
      next("DOM.getBoxModel", base::DictValue().Set("nodeId", node_id));
      return;
    }
    case 7: {
      const base::DictValue* model =
          result.has_value() ? result->FindDict("model") : nullptr;
      state->box = model ? FormatBox(*model)
                         : "Box: none (not rendered: display: none, or not "
                           "in the page).";
      const int node_id = state->node_id;
      next("DOM.resolveNode", base::DictValue()
                                  .Set("nodeId", node_id)
                                  .Set("objectGroup", kInspectGroup));
      return;
    }
    case 8: {
      const std::string* object_id =
          result.has_value() ? result->FindStringByDottedPath("object.objectId")
                             : nullptr;
      if (!object_id) {
        InspectStep(std::move(state), 9, base::DictValue());
        return;
      }
      const std::string id = *object_id;
      next("DOMDebugger.getEventListeners",
           base::DictValue().Set("objectId", id));
      return;
    }
    default:
      break;
  }
  // 9: the listeners, then the answer.
  if (result.has_value()) {
    if (base::ListValue* listeners = result->FindList("listeners")) {
      state->listeners = std::move(*listeners);
    }
  }
  Send("Runtime.releaseObjectGroup",
       base::DictValue().Set("objectGroup", kInspectGroup), base::DoNothing());

  std::string text = base::StrCat(
      {state->node.empty() ? std::string("The element") : state->node, "  (",
       state->selector, ")\n", state->box, "\n\nComputed styles:\n"});
  const bool all = std::ranges::contains(state->properties, "all");
  for (const base::Value& property : state->computed) {
    if (!property.is_dict()) {
      continue;
    }
    const std::string name = Str(property.GetDict(), "name");
    const bool wanted =
        all || (state->properties.empty()
                    ? std::ranges::contains(kDefaultComputed, name)
                    : std::ranges::contains(state->properties, name));
    if (wanted) {
      base::StrAppend(&text, {"  ", name, ": ",
                              fmt::Cut(Str(property.GetDict(), "value"), 200),
                              "\n"});
    }
  }

  text += "\nMatching rules, winning first:\n";
  size_t rules = 0;
  if (const base::DictValue* inline_style =
          state->matched.FindDict("inlineStyle")) {
    const std::string declarations = Declarations(*inline_style);
    if (!declarations.empty()) {
      base::StrAppend(&text, {"  element.style (its style attribute)\n",
                              declarations});
      ++rules;
    }
  }
  if (const base::ListValue* matched =
          state->matched.FindList("matchedCSSRules")) {
    // DevTools gives them in cascade order, the winner last.
    for (auto it = matched->rbegin(); it != matched->rend(); ++it) {
      if (rules == kMaxRules) {
        text += "  … more rules not shown.\n";
        break;
      }
      const base::DictValue* rule =
          it->is_dict() ? it->GetDict().FindDict("rule") : nullptr;
      if (!rule) {
        continue;
      }
      const std::string origin = Str(*rule, "origin");
      if (origin == "user-agent" && !state->user_agent) {
        continue;
      }
      std::string where;
      if (origin == "user-agent") {
        where = "the browser's default";
      } else if (const std::string* sheet_id =
                     rule->FindString("styleSheetId")) {
        auto sheet = style_sheets_.find(*sheet_id);
        const int line =
            static_cast<int>(
                rule->FindDoubleByDottedPath("style.range.startLine")
                    .value_or(0)) +
            1;
        if (sheet == style_sheets_.end()) {
          where = "a style sheet";
        } else if (sheet->second.url.empty()) {
          where = "a style sheet made by a script";
        } else if (sheet->second.is_inline) {
          where = base::StrCat(
              {"<style> in ", sheet->second.url, ", line ",
               base::NumberToString(line + sheet->second.start_line)});
        } else {
          where = base::StrCat(
              {sheet->second.url, ":", base::NumberToString(line)});
        }
      }
      std::string media;
      if (const base::ListValue* medias = rule->FindList("media");
          medias && !medias->empty() && (*medias)[0].is_dict()) {
        media = base::StrCat({" @media ", Str((*medias)[0].GetDict(), "text")});
      }
      const base::DictValue* style = rule->FindDict("style");
      base::StrAppend(
          &text,
          {"  ", rule->FindStringByDottedPath("selectorList.text")
                     ? *rule->FindStringByDottedPath("selectorList.text")
                     : std::string("?"),
           media, where.empty() ? std::string() : "  (" + where + ")", "\n",
           style ? Declarations(*style) : std::string()});
      ++rules;
    }
  }
  if (!rules) {
    text += "  none of the page's own.\n";
  }

  text += "\nEvent listeners on the element:\n";
  if (state->listeners.empty()) {
    text +=
        "  none (listeners on ancestors, like the document, handle its events "
        "too: inspect those).\n";
  }
  for (const base::Value& listener : state->listeners) {
    if (!listener.is_dict()) {
      continue;
    }
    const base::DictValue& dict = listener.GetDict();
    std::vector<std::string> flags;
    if (dict.FindBool("useCapture").value_or(false)) {
      flags.push_back("capture");
    }
    if (dict.FindBool("passive").value_or(false)) {
      flags.push_back("passive");
    }
    if (dict.FindBool("once").value_or(false)) {
      flags.push_back("once");
    }
    std::string handler;
    if (const std::string* description =
            dict.FindStringByDottedPath("handler.description")) {
      handler = fmt::Cut(OneLine(*description), 150);
    }
    base::StrAppend(
        &text,
        {"  ", Str(dict, "type"),
         flags.empty() ? std::string()
                       : base::StrCat({" (", base::JoinString(flags, ", "),
                                       ")"}),
         handler.empty() ? std::string() : " — " + handler, "\n"});
  }
  Finish(ToolResult::Text(fmt::Cut(text, kMaxInspectBytes)));
}

// ---- page_storage ---------------------------------------------------------

struct ClaudeTools::StorageState {
  bool cookies = true;
  bool local = true;
  bool session = true;
  bool indexed_db = true;
  std::string filter;
  bool show_values = false;
  GURL url;
  std::string text;
};

void ClaudeTools::PageStorage(base::DictValue input) {
  auto state = std::make_unique<StorageState>();
  if (const std::string* store = input.FindString("store")) {
    state->cookies = *store == "cookies";
    state->local = *store == "localStorage";
    state->session = *store == "sessionStorage";
    state->indexed_db = *store == "indexedDB";
  }
  state->filter = base::ToLowerASCII(Str(input, "filter"));
  state->show_values = input.FindBool("show_values").value_or(false);
  state->url = web_contents()->GetLastCommittedURL();
  state->text = base::StrCat(
      {"Storage of ", url::Origin::Create(state->url).Serialize(), "\n"});
  StorageStep(std::move(state), 0, base::DictValue());
}

void ClaudeTools::StorageStep(std::unique_ptr<StorageState> state,
                              int step,
                              Result result) {
  auto then = [&](int next_step) {
    return base::BindOnce(&ClaudeTools::StorageStep,
                          run_weak_factory_.GetWeakPtr(), std::move(state),
                          next_step);
  };
  switch (step) {
    case 0:
      if (!state->cookies) {
        StorageStep(std::move(state), 1, base::DictValue());
        return;
      }
      {
        const std::string url = state->url.spec();
        Send("Network.getCookies",
             base::DictValue().Set("urls", base::ListValue().Append(url)),
             then(1));
      }
      return;
    case 1:
      if (state->cookies) {
        if (!result.has_value()) {
          base::StrAppend(&state->text,
                          {"\nCookies: couldn't read them (", result.error(),
                           ").\n"});
        } else {
          std::string lines;
          size_t count = 0;
          if (const base::ListValue* cookies = result->FindList("cookies")) {
            for (const base::Value& cookie : *cookies) {
              if (!cookie.is_dict() ||
                  !Matches(Str(cookie.GetDict(), "name"), state->filter)) {
                continue;
              }
              ++count;
              lines += FormatCookie(cookie.GetDict(), state->show_values);
            }
          }
          base::StrAppend(&state->text,
                          {"\nCookies (", base::NumberToString(count), "):\n",
                           count ? lines : std::string("  none\n")});
        }
      }
      if (!state->local && !state->session) {
        StorageStep(std::move(state), 2, base::DictValue());
        return;
      }
      Agent("storage", base::DictValue(), then(2));
      return;
    case 2:
      if (state->local || state->session) {
        if (!result.has_value()) {
          base::StrAppend(&state->text,
                          {"\nStorage: couldn't read it (", result.error(),
                           ").\n"});
        }
        for (const char* name : {"localStorage", "sessionStorage"}) {
          if (!result.has_value() ||
              (name == std::string_view("localStorage") ? !state->local
                                                        : !state->session)) {
            continue;
          }
          const base::DictValue* store = result->FindDict(name);
          if (!store) {
            continue;
          }
          if (const std::string* error = store->FindString("error")) {
            base::StrAppend(&state->text, {"\n", name, ": the page can't use it (",
                                           *error, ").\n"});
            continue;
          }
          std::string lines;
          size_t shown = 0;
          size_t matching = 0;
          if (const base::ListValue* items = store->FindList("items")) {
            for (const base::Value& item : *items) {
              const base::ListValue* pair = item.GetIfList();
              if (!pair || pair->size() != 2 || !(*pair)[0].is_string()) {
                continue;
              }
              const std::string& key = (*pair)[0].GetString();
              const std::string value =
                  (*pair)[1].is_string() ? (*pair)[1].GetString() : "";
              if (!Matches(key, state->filter)) {
                continue;
              }
              ++matching;
              if (shown == kMaxStorageItems) {
                continue;
              }
              ++shown;
              std::string shown_value;
              if (state->show_values) {
                shown_value = fmt::Cut(value, kMaxShownStorageValue);
              } else if (fmt::IsSecretName(key) || fmt::LooksSecret(value)) {
                shown_value = fmt::Hidden(value);
              } else {
                shown_value =
                    fmt::Cut(fmt::MaskSecretsInText(value), kMaxStorageValue);
              }
              base::StrAppend(&lines, {"  ", key, " = ", shown_value, "\n"});
            }
          }
          base::StrAppend(&state->text,
                          {"\n", name, " (", base::NumberToString(matching),
                           matching == 1 ? " key" : " keys", "):\n",
                           matching ? lines : std::string("  none\n")});
          if (matching > shown) {
            base::StrAppend(&state->text,
                            {"  … ", base::NumberToString(matching - shown),
                             " more: use filter.\n"});
          }
        }
      }
      if (!state->indexed_db) {
        StorageStep(std::move(state), 3, base::DictValue());
        return;
      }
      {
        const std::string origin = url::Origin::Create(state->url).Serialize();
        Send("IndexedDB.requestDatabaseNames",
             base::DictValue().Set("securityOrigin", origin), then(3));
      }
      return;
    default:
      break;
  }
  if (state->indexed_db) {
    std::vector<std::string> names;
    if (result.has_value()) {
      if (const base::ListValue* list = result->FindList("databaseNames")) {
        for (const base::Value& name : *list) {
          if (name.is_string() && Matches(name.GetString(), state->filter)) {
            names.push_back(name.GetString());
          }
        }
      }
      base::StrAppend(&state->text,
                      {"\nIndexedDB databases: ",
                       names.empty() ? std::string("none")
                                     : base::JoinString(names, ", "),
                       "\n"});
    } else {
      base::StrAppend(&state->text, {"\nIndexedDB: couldn't list them (",
                                     result.error(), ").\n"});
    }
  }
  Finish(ToolResult::Text(std::move(state->text)));
}

// ---- edit_page_storage ----------------------------------------------------

void ClaudeTools::EditPageStorage(base::DictValue input) {
  const std::string store = Str(input, "store");
  const std::string action = Str(input, "action");
  const std::string name = Str(input, "name");
  const GURL url = web_contents()->GetLastCommittedURL();
  if (store != "cookies" && store != "localStorage" &&
      store != "sessionStorage") {
    Finish(ToolResult::Error(
        "store must be cookies, localStorage or sessionStorage."));
    return;
  }
  if (action != "set" && action != "remove" && action != "clear") {
    Finish(ToolResult::Error("action must be set, remove or clear."));
    return;
  }
  if (action != "clear" && name.empty()) {
    Finish(ToolResult::Error(base::StrCat({action, " needs a name."})));
    return;
  }
  if (action == "set" && !input.FindString("value")) {
    Finish(ToolResult::Error("set needs a value."));
    return;
  }
  if (store == "cookies" && !url.SchemeIsHTTPOrHTTPS()) {
    Finish(ToolResult::Error("Only web pages (http, https) have cookies."));
    return;
  }
  const std::string host = url.host();
  const std::string where =
      store == "cookies"
          ? std::string()
          : base::StrCat({" in ", host, store == "localStorage"
                                            ? "'s local storage"
                                            : "'s session storage"});
  ApprovalRequest request;
  request.tool = "edit_page_storage";
  if (store == "cookies") {
    request.title =
        action == "set"
            ? base::StrCat({"Set the cookie \u201c", name, "\u201d on ", host})
        : action == "remove"
            ? base::StrCat({"Delete the cookie \u201c", name, "\u201d on ",
                            host})
            : base::StrCat({"Delete all cookies of ", host});
  } else {
    request.title =
        action == "set"
            ? base::StrCat({"Set \u201c", name, "\u201d", where})
        : action == "remove"
            ? base::StrCat({"Remove \u201c", name, "\u201d", where})
            : base::StrCat({"Clear everything", where});
  }
  if (action == "set") {
    request.detail = fmt::Cut(Str(input, "value"), kMaxApprovalValue);
    if (const base::DictValue* cookie = input.FindDict("cookie");
        cookie && store == "cookies") {
      std::string json;
      base::JSONWriter::Write(*cookie, &json);
      base::StrAppend(&request.detail, {"\n", json});
    }
  }
  ApproveThen(std::move(request),
              base::BindOnce(&ClaudeTools::RunStorageEdit,
                             run_weak_factory_.GetWeakPtr(), std::move(input)));
}

void ClaudeTools::RunStorageEdit(base::DictValue input) {
  if (!web_contents()) {
    Finish(ToolResult::Error("The tab Claude was using closed."));
    return;
  }
  const std::string store = Str(input, "store");
  const std::string action = Str(input, "action");
  const std::string name = Str(input, "name");
  const std::string value = Str(input, "value");
  const std::string url = web_contents()->GetLastCommittedURL().spec();
  // What answers the change: |text| when it worked.
  auto done = [this](std::string text) {
    return base::BindOnce(
        [](base::WeakPtr<ClaudeTools> self, std::string text, Result result) {
          if (!self) {
            return;
          }
          if (!result.has_value()) {
            self->Finish(ToolResult::Error(result.error()));
            return;
          }
          if (!result->FindBool("success").value_or(true)) {
            self->Finish(ToolResult::Error(
                "The browser refused the cookie: check its domain and path, "
                "and Secure (needed with SameSite=None, and on https)."));
            return;
          }
          self->Finish(ToolResult::Text(std::move(text)));
        },
        run_weak_factory_.GetWeakPtr(), std::move(text));
  };

  if (store != "cookies") {
    std::string text =
        action == "set"      ? base::StrCat({"Set \u201c", name, "\u201d in ",
                                             store, "."})
        : action == "remove" ? base::StrCat({"Removed \u201c", name,
                                             "\u201d from ", store, "."})
                             : base::StrCat({"Cleared ", store, "."});
    Agent("editStorage",
          base::DictValue()
              .Set("store", store)
              .Set("action", action)
              .Set("name", name)
              .Set("value", value),
          done(std::move(text)));
    return;
  }
  if (action == "set") {
    base::DictValue params;
    params.Set("name", name);
    params.Set("value", value);
    params.Set("url", url);
    if (const base::DictValue* cookie = input.FindDict("cookie")) {
      if (const std::string* domain = cookie->FindString("domain")) {
        params.Set("domain", *domain);
      }
      params.Set("path", cookie->FindString("path") ? Str(*cookie, "path")
                                                     : std::string("/"));
      if (std::optional<double> expires = cookie->FindDouble("expires")) {
        params.Set("expires", *expires);
      }
      if (std::optional<bool> secure = cookie->FindBool("secure")) {
        params.Set("secure", *secure);
      }
      if (std::optional<bool> http_only = cookie->FindBool("http_only")) {
        params.Set("httpOnly", *http_only);
      }
      if (const std::string* same_site = cookie->FindString("same_site")) {
        params.Set("sameSite", *same_site);
      }
    } else {
      params.Set("path", "/");
    }
    Send("Network.setCookie", std::move(params),
         done(base::StrCat({"Set the cookie \u201c", name, "\u201d."})));
    return;
  }
  if (action == "remove") {
    base::DictValue params;
    params.Set("name", name);
    params.Set("url", url);
    if (const base::DictValue* cookie = input.FindDict("cookie")) {
      if (const std::string* domain = cookie->FindString("domain")) {
        params.Set("domain", *domain);
      }
      if (const std::string* path = cookie->FindString("path")) {
        params.Set("path", *path);
      }
    }
    Send("Network.deleteCookies", std::move(params),
         done(base::StrCat({"Deleted the cookie \u201c", name, "\u201d."})));
    return;
  }
  // clear: every cookie the page gets, each deleted where it lives.
  Send("Network.getCookies",
       base::DictValue().Set("urls", base::ListValue().Append(url)),
       base::BindOnce(
           [](base::WeakPtr<ClaudeTools> self, std::string url,
              Result cookies) {
             if (!self) {
               return;
             }
             if (!cookies.has_value()) {
               self->Finish(ToolResult::Error(cookies.error()));
               return;
             }
             std::vector<std::pair<std::string, base::DictValue>> commands;
             if (const base::ListValue* list = cookies->FindList("cookies")) {
               for (const base::Value& cookie : *list) {
                 if (!cookie.is_dict()) {
                   continue;
                 }
                 commands.emplace_back(
                     "Network.deleteCookies",
                     base::DictValue()
                         .Set("name", Str(cookie.GetDict(), "name"))
                         .Set("domain", Str(cookie.GetDict(), "domain"))
                         .Set("path", Str(cookie.GetDict(), "path")));
               }
             }
             const size_t count = commands.size();
             self->SendAll(
                 std::move(commands),
                 base::BindOnce(
                     [](base::WeakPtr<ClaudeTools> self, size_t count,
                        Result result) {
                       if (!self) {
                         return;
                       }
                       self->Finish(
                           result.has_value()
                               ? ToolResult::Text(base::StrCat(
                                     {"Deleted ", base::NumberToString(count),
                                      count == 1 ? " cookie." : " cookies."}))
                               : ToolResult::Error(result.error()));
                     },
                     self->run_weak_factory_.GetWeakPtr(), count));
           },
           run_weak_factory_.GetWeakPtr(), url));
}


// ---- performance ----------------------------------------------------------

void ClaudeTools::Performance(base::DictValue input) {
  Send("Performance.enable", base::DictValue(),
       base::BindOnce(
           [](base::WeakPtr<ClaudeTools> self, Result enabled) {
             if (!self) {
               return;
             }
             self->Send(
                 "Performance.getMetrics", base::DictValue(),
                 base::BindOnce(
                     [](base::WeakPtr<ClaudeTools> self, Result metrics) {
                       if (!self) {
                         return;
                       }
                       base::DictValue runtime;
                       if (metrics.has_value()) {
                         runtime = std::move(*metrics);
                       }
                       self->Send(
                           "Runtime.evaluate",
                           base::DictValue()
                               .Set("expression", kVitalsScript)
                               .Set("awaitPromise", true)
                               .Set("returnByValue", true)
                               .Set("timeout", 10000),
                           base::BindOnce(
                               [](base::WeakPtr<ClaudeTools> self,
                                  base::DictValue runtime, Result evaluated) {
                                 if (!self) {
                                   return;
                                 }
                                 const base::DictValue* vitals =
                                     evaluated.has_value()
                                         ? evaluated->FindDictByDottedPath(
                                               "result.value")
                                         : nullptr;
                                 std::string text;
                                 if (vitals) {
                                   const base::DictValue& v = *vitals;
                                   auto num = [&](const char* key) {
                                     return v.FindDouble(key).value_or(-1);
                                   };
                                   text = base::StrCat(
                                       {"Performance of ", Str(v, "url"),
                                        "\n\nLoading:\n"});
                                   const double ttfb = num("ttfb");
                                   base::StrAppend(
                                       &text,
                                       {"  Time to first byte: ", Ms(ttfb),
                                        ttfb >= 0 ? " (" +
                                                        Rate(ttfb, 800, 1800) +
                                                        ")"
                                                  : "",
                                        "\n"});
                                   const double fcp = num("fcp");
                                   base::StrAppend(
                                       &text,
                                       {"  First contentful paint: ", Ms(fcp),
                                        fcp >= 0 ? " (" +
                                                       Rate(fcp, 1800, 3000) +
                                                       ")"
                                                 : "",
                                        "\n"});
                                   const double lcp = num("lcp");
                                   const std::string lcp_element =
                                       Str(v, "lcpElement");
                                   base::StrAppend(
                                       &text,
                                       {"  Largest contentful paint: ", Ms(lcp),
                                        lcp >= 0 ? " (" +
                                                       Rate(lcp, 2500, 4000) +
                                                       ")"
                                                 : "",
                                        lcp_element.empty()
                                            ? ""
                                            : ", " + lcp_element,
                                        "\n"});
                                   base::StrAppend(
                                       &text, {"  DOMContentLoaded: ",
                                               Ms(num("dcl")), "; load: ",
                                               Ms(num("load")), "\n"});
                                   const double cls =
                                       std::max(0.0, num("cls"));
                                   base::StrAppend(
                                       &text,
                                       {"\nStability and responsiveness:\n",
                                        base::StringPrintf(
                                            "  Cumulative layout shift: %.3f",
                                            cls),
                                        " (", Rate(cls, 0.1, 0.25), ")\n"});
                                   const double inp = num("inp");
                                   const int interactions =
                                       v.FindInt("interactions").value_or(0);
                                   base::StrAppend(
                                       &text,
                                       {"  Interaction to next paint: ",
                                        inp >= 0
                                            ? base::StrCat(
                                                  {Ms(inp), " (",
                                                   Rate(inp, 200, 500),
                                                   "), the slowest of ",
                                                   base::NumberToString(
                                                       interactions),
                                                   interactions == 1
                                                       ? " interaction"
                                                       : " interactions"})
                                            : std::string(
                                                  "no interactions yet"),
                                        "\n"});
                                   base::StrAppend(
                                       &text,
                                       {"  Long animation frames: ",
                                        base::NumberToString(
                                            v.FindInt("longFrames")
                                                .value_or(0)),
                                        ", blocking ",
                                        Ms(std::max(0.0, num("blocking"))),
                                        "\n"});
                                   base::StrAppend(
                                       &text,
                                       {"\nResources: ",
                                        base::NumberToString(
                                            v.FindInt("resources")
                                                .value_or(0)),
                                        ", ",
                                        fmt::FormatBytes(
                                            std::max(0.0, num("transfer"))),
                                        " transferred"});
                                   if (const base::ListValue* slowest =
                                           v.FindList("slowest");
                                       slowest && !slowest->empty()) {
                                     text += "; the slowest:\n";
                                     for (const base::Value& r : *slowest) {
                                       if (!r.is_dict()) {
                                         continue;
                                       }
                                       const base::DictValue& d = r.GetDict();
                                       base::StrAppend(
                                           &text,
                                           {"  ",
                                            Ms(d.FindDouble("duration")
                                                   .value_or(0)),
                                            "  ", Str(d, "type"), "  ",
                                            fmt::Cut(fmt::MaskSecretsInText(
                                                         Str(d, "url")),
                                                     kMaxListedUrl),
                                            "\n"});
                                     }
                                   } else {
                                     text += "\n";
                                   }
                                 } else {
                                   text = base::StrCat(
                                       {"The page's timings couldn't be read",
                                        evaluated.has_value()
                                            ? std::string()
                                            : " (" + evaluated.error() + ")",
                                        ".\n"});
                                 }
                                 const base::ListValue* metrics =
                                     runtime.FindList("metrics");
                                 if (metrics) {
                                   base::StrAppend(
                                       &text,
                                       {"\nNow:\n  JavaScript heap: ",
                                        fmt::FormatBytes(Metric(
                                            metrics, "JSHeapUsedSize")),
                                        " used of ",
                                        fmt::FormatBytes(Metric(
                                            metrics, "JSHeapTotalSize")),
                                        "\n",
                                        base::StringPrintf(
                                            "  DOM nodes: %.0f; event "
                                            "listeners: %.0f; frames: %.0f\n",
                                            Metric(metrics, "Nodes"),
                                            Metric(metrics,
                                                   "JSEventListeners"),
                                            Metric(metrics, "Frames")),
                                        base::StringPrintf(
                                            "  Layouts: %.0f (",
                                            Metric(metrics, "LayoutCount")),
                                        fmt::FormatDuration(Metric(
                                            metrics, "LayoutDuration")),
                                        base::StringPrintf(
                                            "); style recalculations: %.0f (",
                                            Metric(metrics,
                                                   "RecalcStyleCount")),
                                        fmt::FormatDuration(Metric(
                                            metrics, "RecalcStyleDuration")),
                                        "); script: ",
                                        fmt::FormatDuration(Metric(
                                            metrics, "ScriptDuration")),
                                        "; all tasks: ",
                                        fmt::FormatDuration(Metric(
                                            metrics, "TaskDuration")),
                                        "\n"});
                                 }
                                 self->Finish(ToolResult::Text(
                                     std::move(text)));
                               },
                               self->run_weak_factory_.GetWeakPtr(),
                               std::move(runtime)));
                     },
                     self->run_weak_factory_.GetWeakPtr()));
           },
           run_weak_factory_.GetWeakPtr()));
}

// ---- emulate --------------------------------------------------------------

void ClaudeTools::Emulate(base::DictValue input) {
  if (input.FindBool("reset").value_or(false)) {
    // Letting go of the session undoes every override it made; the next
    // tool attaches again.
    const bool any = !emulating_.empty();
    ResetDevTools();
    Finish(ToolResult::Text(any ? "Emulation undone: the tab is as it was."
                                : "Nothing was emulated."));
    return;
  }
  std::vector<std::pair<std::string, base::DictValue>> commands;

  // The viewport: a device, or a size.
  const std::string* device_name = input.FindString("device");
  const DevicePreset* device = nullptr;
  if (device_name) {
    for (const DevicePreset& preset : kDevices) {
      if (*device_name == preset.name) {
        device = &preset;
      }
    }
    if (!device) {
      Finish(ToolResult::Error(
          "device must be iphone, android, ipad or laptop."));
      return;
    }
  }
  std::optional<int> width = input.FindInt("width");
  std::optional<int> height = input.FindInt("height");
  if (device || width || height) {
    const int w = std::clamp(width.value_or(device ? device->width : 1280),
                             200, 4000);
    const int h = std::clamp(height.value_or(device ? device->height : 800),
                             200, 4000);
    const double scale = std::clamp(
        input.FindDouble("scale").value_or(device ? device->scale : 1), 0.5,
        4.0);
    const bool mobile =
        input.FindBool("mobile").value_or(device && device->mobile);
    commands.emplace_back("Emulation.setDeviceMetricsOverride",
                          base::DictValue()
                              .Set("width", w)
                              .Set("height", h)
                              .Set("deviceScaleFactor", scale)
                              .Set("mobile", mobile));
    commands.emplace_back("Emulation.setTouchEmulationEnabled",
                          base::DictValue().Set("enabled", mobile));
    emulating_["viewport"] = base::StrCat(
        {device ? std::string(device->label) + ", " : std::string(),
         base::StringPrintf("%d×%d at %gx", w, h, scale),
         mobile ? ", touch" : ""});
  }
  // The user agent: given, or the device's.
  std::string user_agent = Str(input, "user_agent");
  if (user_agent.empty() && device) {
    user_agent = device->user_agent;
  }
  if (!user_agent.empty()) {
    commands.emplace_back("Emulation.setUserAgentOverride",
                          base::DictValue().Set("userAgent", user_agent));
    emulating_["user agent"] = fmt::Cut(user_agent, 80);
  }

  // Media: they go together.
  bool media_changed = false;
  if (const std::string* scheme = input.FindString("color_scheme")) {
    if (*scheme != "light" && *scheme != "dark") {
      Finish(ToolResult::Error("color_scheme must be light or dark."));
      return;
    }
    emulated_color_scheme_ = *scheme;
    emulating_["color scheme"] = *scheme;
    media_changed = true;
  }
  if (std::optional<bool> reduced = input.FindBool("reduced_motion")) {
    emulated_reduced_motion_ = *reduced;
    if (*reduced) {
      emulating_["reduced motion"] = "on";
    } else {
      emulating_.erase("reduced motion");
    }
    media_changed = true;
  }
  if (const std::string* media = input.FindString("media")) {
    if (*media != "print" && *media != "screen") {
      Finish(ToolResult::Error("media must be print or screen."));
      return;
    }
    emulated_media_type_ = *media == "print" ? "print" : "";
    if (*media == "print") {
      emulating_["media"] = "print";
    } else {
      emulating_.erase("media");
    }
    media_changed = true;
  }
  if (media_changed) {
    base::ListValue features;
    if (!emulated_color_scheme_.empty()) {
      features.Append(base::DictValue()
                          .Set("name", "prefers-color-scheme")
                          .Set("value", emulated_color_scheme_));
    }
    if (emulated_reduced_motion_) {
      features.Append(base::DictValue()
                          .Set("name", "prefers-reduced-motion")
                          .Set("value", "reduce"));
    }
    commands.emplace_back("Emulation.setEmulatedMedia",
                          base::DictValue()
                              .Set("media", emulated_media_type_)
                              .Set("features", std::move(features)));
  }

  // The network and the CPU.
  if (const std::string* network = input.FindString("network")) {
    const NetworkPreset* preset = nullptr;
    for (const NetworkPreset& candidate : kNetworks) {
      if (*network == candidate.name) {
        preset = &candidate;
      }
    }
    if (!preset) {
      Finish(ToolResult::Error(
          "network must be offline, slow_3g, fast_3g, slow_4g or none."));
      return;
    }
    commands.emplace_back("Network.emulateNetworkConditions",
                          base::DictValue()
                              .Set("offline", preset->offline)
                              .Set("latency", preset->latency_ms)
                              .Set("downloadThroughput", preset->download)
                              .Set("uploadThroughput", preset->upload));
    if (*network == "none") {
      emulating_.erase("network");
    } else {
      emulating_["network"] = preset->name;
    }
  }
  if (std::optional<double> slowdown = input.FindDouble("cpu_slowdown")) {
    const double rate = std::clamp(*slowdown, 1.0, 20.0);
    commands.emplace_back("Emulation.setCPUThrottlingRate",
                          base::DictValue().Set("rate", rate));
    if (rate > 1) {
      emulating_["CPU"] = base::StringPrintf("%gx slower", rate);
    } else {
      emulating_.erase("CPU");
    }
  }
  if (commands.empty()) {
    Finish(ToolResult::Error(
        "Say what to emulate: a device, width and height, color_scheme, "
        "reduced_motion, media, network, cpu_slowdown or user_agent; or "
        "reset."));
    return;
  }
  const bool reload_needed = !user_agent.empty();
  SendAll(std::move(commands),
          base::BindOnce(
              [](base::WeakPtr<ClaudeTools> self, bool reload_needed,
                 Result result) {
                if (!self) {
                  return;
                }
                if (!result.has_value()) {
                  self->Finish(ToolResult::Error(result.error()));
                  return;
                }
                std::vector<std::string> parts;
                for (const auto& [what, how] : self->emulating_) {
                  parts.push_back(base::StrCat({what, ": ", how}));
                }
                std::string text = base::StrCat(
                    {"Emulating ", base::JoinString(parts, "; "),
                     ". It lasts while you work on this tab: it's undone "
                     "when the chat stops, a minute after your last tool, "
                     "or with reset."});
                if (reload_needed) {
                  text += " The user agent applies to new requests: navigate "
                          "with \"reload\" to load the page with it.";
                }
                self->Finish(ToolResult::Text(std::move(text)));
              },
              run_weak_factory_.GetWeakPtr(), reload_needed));
}

}  // namespace compass::claude
