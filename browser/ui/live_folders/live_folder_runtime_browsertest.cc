// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Runs the plugin cases (plugins/builtin/*/tests/*, see
// docs/design/live-folders.md, "Testing") through the desktop runtime: the
// hidden chrome-untrusted://live-folders/ page, a Worker per refresh, and the
// browser's fetches, answered from the cases' recorded responses. Each case
// must come out as its expected.json says, as it does on the Node reference
// runtime (tools/live_folders/run.mjs).

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "base/memory/raw_ptr.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "compass/browser/ui/live_folders/preview_session_fetcher.h"
#include "net/cookies/site_for_cookies.h"
#include "net/url_request/redirect_info.h"
#include "compass/browser/ui/live_folders/live_folder_host.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "content/public/test/browser_test.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"

namespace compass {
namespace {

std::optional<base::Value> ReadJson(const base::FilePath& path) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  return base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
}

// "2026-11-09T08:30:00Z", as the cases give "now".
std::optional<double> ParseUtc(const std::string& text) {
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
      text[19] != 'Z') {
    return std::nullopt;
  }
  auto number = [&text](size_t at, size_t length) {
    int value = 0;
    return base::StringToInt(std::string_view(text).substr(at, length),
                             &value)
               ? value
               : -1;
  };
  base::Time::Exploded exploded = {};
  exploded.year = number(0, 4);
  exploded.month = number(5, 2);
  exploded.day_of_month = number(8, 2);
  exploded.hour = number(11, 2);
  exploded.minute = number(14, 2);
  exploded.second = number(17, 2);
  base::Time time;
  if (!base::Time::FromUTCExploded(exploded, &time)) {
    return std::nullopt;
  }
  return time.InMillisecondsFSinceUnixEpoch();
}

base::FilePath PluginsDir() {
  base::FilePath root;
  base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root);
  return root.AppendASCII("compass").AppendASCII("plugins");
}

struct Case {
  std::string name;
  base::FilePath dir;
  // The built-in plugin it runs.
  std::string plugin_id;
  // Its plugin's manifest is invalid on purpose.
  bool bad_manifest = false;
};

std::vector<Case> BuiltinCases() {
  base::ScopedAllowBlockingForTesting allow_blocking;
  std::vector<Case> cases;
  base::FileEnumerator plugins(PluginsDir().AppendASCII("builtin"), false,
                               base::FileEnumerator::DIRECTORIES);
  for (base::FilePath plugin = plugins.Next(); !plugin.empty();
       plugin = plugins.Next()) {
    std::optional<base::Value> manifest =
        ReadJson(plugin.AppendASCII("manifest.json"));
    const std::string* id =
        manifest && manifest->is_dict() ? manifest->GetDict().FindString("id")
                                        : nullptr;
    if (!id) {
      continue;
    }
    base::FileEnumerator tests(plugin.AppendASCII("tests"), false,
                               base::FileEnumerator::DIRECTORIES);
    for (base::FilePath dir = tests.Next(); !dir.empty(); dir = tests.Next()) {
      cases.push_back({plugin.BaseName().MaybeAsASCII() + "/" +
                           dir.BaseName().MaybeAsASCII(),
                       dir, *id});
    }
  }
  return cases;
}

// A case's recorded response for |method| |url|, or null.
const base::DictValue* FindResponse(const base::DictValue& responses,
                                    const std::string& method,
                                    const std::string& url) {
  const base::DictValue* entry = responses.FindDict(method + " " + url);
  if (!entry && method == "GET") {
    entry = responses.FindDict(url);
  }
  return entry;
}

std::string ResponseBody(const base::DictValue& entry,
                         const base::FilePath& dir) {
  std::string body;
  if (const std::string* file = entry.FindString("file")) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::ReadFileToString(dir.AppendASCII(*file), &body);
  } else if (const base::Value* json = entry.Find("json")) {
    body = base::WriteJson(*json).value_or("");
  } else if (const std::string* text = entry.FindString("body")) {
    body = *text;
  }
  return body;
}

network::mojom::URLResponseHeadPtr ResponseHead(const base::DictValue& entry) {
  std::string raw_headers =
      "HTTP/1.1 " +
      base::NumberToString(entry.FindInt("status").value_or(200)) + " OK\n";
  if (const base::DictValue* headers = entry.FindDict("headers")) {
    for (const auto [name, value] : *headers) {
      if (value.is_string()) {
        raw_headers += name + ": " + value.GetString() + "\n";
      }
    }
  }
  auto head = network::mojom::URLResponseHead::New();
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(raw_headers));
  std::string charset;
  if (head->headers->GetCharset(&charset)) {
    head->charset = charset;
  }
  return head;
}

// A redirect entry's target: {status: 3xx, headers: {location}}.
std::optional<GURL> RedirectTarget(const base::DictValue& entry,
                                   const GURL& url) {
  const int status = entry.FindInt("status").value_or(200);
  const base::DictValue* headers = entry.FindDict("headers");
  const std::string* location =
      headers ? headers->FindString("location") : nullptr;
  if (status < 300 || status >= 400 || !location) {
    return std::nullopt;
  }
  return url.Resolve(*location);
}

// Answers the case's recorded responses: {"<url>" | "POST <url>": {status,
// headers, file | body | json, rejectHeaders}}, as fixtureFetcher() in
// tools/live_folders/runtime.mjs does. A 3xx entry with a location is a
// redirect, followed to the entry it names.
void ServeResponses(network::TestURLLoaderFactory& factory,
                    const base::DictValue& responses,
                    const base::FilePath& dir) {
  factory.SetInterceptor(base::BindRepeating(
      [](network::TestURLLoaderFactory* factory,
         const base::DictValue* responses, base::FilePath dir,
         const network::ResourceRequest& request) {
        auto fail = [&] {
          factory->AddResponse(request.url,
                               network::mojom::URLResponseHead::New(), "",
                               network::URLLoaderCompletionStatus(
                                   net::ERR_CONNECTION_REFUSED));
        };
        network::TestURLLoaderFactory::Redirects redirects;
        GURL url = request.url;
        std::string method = request.method;
        const base::DictValue* entry =
            FindResponse(*responses, method, url.spec());
        while (entry) {
          std::optional<GURL> next = RedirectTarget(*entry, url);
          if (!next || redirects.size() > 10) {
            break;
          }
          net::RedirectInfo info;
          info.status_code = entry->FindInt("status").value_or(302);
          if (info.status_code == 303 ||
              ((info.status_code == 301 || info.status_code == 302) &&
               method == "POST")) {
            method = "GET";
          }
          info.new_method = method;
          info.new_url = *next;
          info.new_site_for_cookies = net::SiteForCookies::FromUrl(*next);
          redirects.emplace_back(info, ResponseHead(*entry));
          url = *next;
          entry = FindResponse(*responses, method, url.spec());
        }
        if (!entry) {
          return fail();
        }
        if (const base::ListValue* reject = entry->FindList("rejectHeaders")) {
          for (const base::Value& header : *reject) {
            if (header.is_string() &&
                request.headers.HasHeader(header.GetString())) {
              return fail();
            }
          }
        }
        factory->AddResponse(request.url, ResponseHead(*entry),
                             ResponseBody(*entry, dir),
                             network::URLLoaderCompletionStatus(),
                             std::move(redirects));
      },
      &factory, &responses, dir));
}

// The tab's side of a preview's session requests, answered from the case's
// recorded responses as the tab would: a redirect comes back opaque.
class FixturePageTransport : public PreviewPageTransport {
 public:
  FixturePageTransport(const base::DictValue* responses, base::FilePath dir)
      : responses_(responses), dir_(std::move(dir)) {}

  void Fetch(const LiveFetchRequest& request,
             size_t max_response_bytes,
             Callback callback) override {
    const base::DictValue* entry =
        FindResponse(*responses_, request.method, request.url);
    if (!entry) {
      std::move(callback).Run(base::unexpected(
          LiveError{"network", "No fixture for " + request.url}));
      return;
    }
    Response response;
    if (RedirectTarget(*entry, GURL(request.url))) {
      response.redirect = true;
      std::move(callback).Run(std::move(response));
      return;
    }
    response.response.status = entry->FindInt("status").value_or(200);
    response.response.url = GURL(request.url);
    if (const base::DictValue* headers = entry->FindDict("headers")) {
      for (const auto [name, value] : *headers) {
        if (value.is_string()) {
          response.response.headers[base::ToLowerASCII(name)] =
              value.GetString();
        }
      }
    }
    response.response.body = ResponseBody(*entry, dir_);
    std::move(callback).Run(std::move(response));
  }

 private:
  const raw_ptr<const base::DictValue> responses_;
  const base::FilePath dir_;
};

class LiveFolderRuntimeBrowserTest : public InProcessBrowserTest {
 protected:
  // The case's outcome as run.mjs prints it: {ok, result | error: {code},
  // fetches: [{method, url, cookies}]}.
  base::Value Run(const Case& c) {
    std::optional<base::Value> spec_value =
        ReadJson(c.dir.AppendASCII("case.json"));
    EXPECT_TRUE(spec_value && spec_value->is_dict()) << c.name;
    const base::DictValue& spec = spec_value->GetDict();

    if (c.bad_manifest) {
      return base::Value(
          base::DictValue()
              .Set("ok", false)
              .Set("error", base::DictValue().Set("code", "bad_manifest"))
              .Set("fetches", base::ListValue()));
    }
    const LivePlugin* plugin = LivePluginRegistry::Get().Find(c.plugin_id);
    EXPECT_TRUE(plugin) << c.plugin_id;

    LiveSecrets secrets;
    if (const base::DictValue* s = spec.FindDict("secrets")) {
      for (const auto [key, value] : *s) {
        if (value.is_string()) {
          secrets[key] = value.GetString();
        }
      }
    }
    base::DictValue env;
    const base::DictValue* case_env = spec.FindDict("env");
    double now = base::Time::Now().InMillisecondsFSinceUnixEpoch();
    if (const base::Value* value = case_env ? case_env->Find("now") : nullptr) {
      if (value->is_string()) {
        std::optional<double> parsed = ParseUtc(value->GetString());
        EXPECT_TRUE(parsed) << value->GetString();
        now = parsed.value_or(now);
      } else if (value->is_int() || value->is_double()) {
        now = value->GetDouble();
      }
    }
    const std::string* locale =
        case_env ? case_env->FindString("locale") : nullptr;
    const std::string* time_zone =
        case_env ? case_env->FindString("timeZone") : nullptr;
    env.Set("now", now);
    env.Set("locale", locale ? *locale : "en-US");
    // run.mjs runs in UTC.
    env.Set("timeZone", time_zone ? *time_zone : "UTC");

    LiveFetcher::Limits limits;
    base::TimeDelta timeout = base::Seconds(30);
    std::optional<int> max_items;
    if (const base::DictValue* l = spec.FindDict("limits")) {
      max_items = l->FindInt("maxItems");
      if (std::optional<int> ms = l->FindInt("timeoutMs")) {
        timeout = base::Milliseconds(*ms);
      }
      if (std::optional<int> n = l->FindInt("maxFetches")) {
        limits.max_fetches = *n;
      }
      if (std::optional<int> n = l->FindInt("maxResponseBytes")) {
        limits.max_response_bytes = *n;
      }
    }

    network::TestURLLoaderFactory factory;
    const base::DictValue* responses = spec.FindDict("responses");
    base::DictValue empty;
    ServeResponses(factory, responses ? *responses : empty, c.dir);

    const base::DictValue* config = spec.FindDict("config");
    // A tab preview case: {"mode": "preview", "page" | "pageFile",
    // "grantSession"}.
    const std::string* mode = spec.FindString("mode");
    const bool preview = mode && *mode == "preview";
    base::expected<LiveFolderHost::RunSpec, LiveError> run_spec =
        base::unexpected(LiveError{"harness", ""});
    if (preview) {
      std::optional<base::Value> page_file;
      const base::DictValue* page = spec.FindDict("page");
      if (const std::string* file = spec.FindString("pageFile")) {
        page_file = ReadJson(c.dir.AppendASCII(*file));
        page = page_file && page_file->is_dict() ? &page_file->GetDict()
                                                 : nullptr;
      }
      const bool live = page && page->FindBool("live").value_or(true);
      run_spec = MakeLivePreviewRunSpec(
          *plugin, page ? *page : empty, config ? *config : empty,
          spec.FindBool("grantSession").value_or(false), std::move(env),
          base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
              &factory),
          live ? std::make_unique<FixturePageTransport>(
                     responses ? responses : &empty, c.dir)
               : nullptr);
    } else {
      run_spec = MakeLiveRunSpec(
          *plugin, config ? *config : empty, secrets,
          spec.FindBool("grantCookies").value_or(false), std::move(env),
          base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
              &factory),
          limits, timeout);
    }
    if (run_spec.has_value() && max_items) {
      run_spec->max_items = *max_items;
    }

    base::DictValue outcome;
    base::ListValue fetches;
    if (!run_spec.has_value()) {
      outcome.Set("ok", false);
      outcome.Set("error",
                  base::DictValue().Set("code", run_spec.error().code));
      outcome.Set("fetches", std::move(fetches));
      return base::Value(std::move(outcome));
    }
    // The fetcher goes with the refresh: its record is taken as it ends. A
    // preview without the session has none.
    LiveFetchClient* fetcher = run_spec->fetcher.get();
    std::vector<LiveFetchRecord> records;
    base::test::TestFuture<base::expected<std::string, LiveError>,
                           std::vector<std::string>>
        done;
    LiveFolderService* service = LiveFolderService::Get(browser()->GetProfile());
    EXPECT_TRUE(service);
    service->host().Run(
        std::move(*run_spec),
        base::BindOnce(
            [](LiveFetchClient* fetcher, std::vector<LiveFetchRecord>* records,
               base::OnceCallback<void(
                   base::expected<std::string, LiveError>,
                   std::vector<std::string>)> callback,
               base::expected<std::string, LiveError> result,
               std::vector<std::string> log) {
              if (fetcher) {
                *records = fetcher->fetches();
              }
              std::move(callback).Run(std::move(result), std::move(log));
            },
            fetcher, &records, done.GetCallback()));
    auto [result, log] = done.Take();

    outcome.Set("ok", result.has_value());
    if (result.has_value()) {
      std::optional<base::Value> parsed =
          base::JSONReader::Read(*result, base::JSON_PARSE_RFC);
      outcome.Set("result", parsed ? std::move(*parsed) : base::Value());
    } else {
      outcome.Set("error", base::DictValue().Set("code", result.error().code));
    }
    for (const LiveFetchRecord& record : records) {
      base::DictValue fetch;
      fetch.Set("method", record.method);
      fetch.Set("url", record.url.spec());
      fetch.Set("cookies", record.cookies);
      if (!record.via.empty()) {
        fetch.Set("via", record.via);
      }
      if (record.redirect) {
        fetch.Set("redirect", true);
      }
      fetches.Append(std::move(fetch));
    }
    outcome.Set("fetches", std::move(fetches));
    return base::Value(std::move(outcome));
  }
};

// The runtime's rules (plugins/conformance/*), each with its own plugin,
// installed as a user's would be: from disk, served on
// chrome-untrusted://live-folders/installed/.
std::vector<Case> InstallConformanceCases(const base::FilePath& plugins_dir) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  std::vector<Case> cases;
  base::FileEnumerator dirs(PluginsDir().AppendASCII("conformance"), false,
                            base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = dirs.Next(); !dir.empty(); dir = dirs.Next()) {
    const base::FilePath plugin = dir.AppendASCII("plugin");
    std::string manifest_json;
    std::string entry;
    if (!base::ReadFileToString(plugin.AppendASCII("manifest.json"),
                                &manifest_json) ||
        !base::ReadFileToString(plugin.AppendASCII("main.js"), &entry)) {
      continue;
    }
    // A case whose manifest is wrong on purpose fails as run.mjs's
    // loadPlugin() does.
    std::optional<base::Value> raw =
        base::JSONReader::Read(manifest_json, base::JSON_PARSE_RFC);
    if (!raw || !ParseLivePluginManifest(*raw).has_value()) {
      cases.push_back({"conformance/" + dir.BaseName().MaybeAsASCII(), dir,
                       std::string(), /*bad_manifest=*/true});
      continue;
    }
    auto package = MakeLivePluginPackage(
        GURL("https://plugins.test/" + dir.BaseName().MaybeAsASCII() +
             "/manifest.json"),
        std::move(manifest_json), std::move(entry), std::string());
    EXPECT_TRUE(package.has_value()) << dir;
    if (!package.has_value()) {
      continue;
    }
    EXPECT_TRUE(WriteLivePluginPackage(plugins_dir, *package));
    LivePluginRegistry::Get().AddInstalled(*package);
    cases.push_back({"conformance/" + dir.BaseName().MaybeAsASCII(), dir,
                     package->manifest.id});
  }
  return cases;
}

IN_PROC_BROWSER_TEST_F(LiveFolderRuntimeBrowserTest, ConformanceCases) {
  LiveFolderService* service = LiveFolderService::Get(browser()->GetProfile());
  ASSERT_TRUE(service);
  std::vector<Case> cases = InstallConformanceCases(service->plugins_dir());
  ASSERT_GE(cases.size(), 22u);
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    std::optional<base::Value> expected =
        ReadJson(c.dir.AppendASCII("expected.json"));
    ASSERT_TRUE(expected) << c.name;
    base::Value actual = Run(c);
    EXPECT_EQ(actual, *expected)
        << c.name << "\n  actual: " << base::WriteJson(actual).value_or("")
        << "\n  expected: " << base::WriteJson(*expected).value_or("");
  }
}

IN_PROC_BROWSER_TEST_F(LiveFolderRuntimeBrowserTest, BuiltinCases) {
  std::vector<Case> cases = BuiltinCases();
  ASSERT_GE(cases.size(), 21u);
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    std::optional<base::Value> expected =
        ReadJson(c.dir.AppendASCII("expected.json"));
    ASSERT_TRUE(expected) << c.name;
    base::Value actual = Run(c);
    EXPECT_EQ(actual, *expected)
        << c.name << "\n  actual: " << base::WriteJson(actual).value_or("")
        << "\n  expected: " << base::WriteJson(*expected).value_or("");
  }
}

}  // namespace
}  // namespace compass
