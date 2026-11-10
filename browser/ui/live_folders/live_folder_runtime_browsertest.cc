// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Runs the plugin cases (plugins/builtin/*/tests/*, see
// docs/design/live-folders.md, "Testing") through the desktop runtime: the
// hidden chrome-untrusted://live-folders/ page, a Worker per refresh, and the
// browser's fetches, answered from the cases' recorded responses. Each case
// must come out as its expected.json says, as it does on the Node reference
// runtime (tools/live_folders/run.mjs).

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
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
  base::Time::Exploded exploded = {};
  char z = 0;
  if (sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &exploded.year,
             &exploded.month, &exploded.day_of_month, &exploded.hour,
             &exploded.minute, &exploded.second, &z) != 7 ||
      z != 'Z') {
    return std::nullopt;
  }
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

// Answers the case's recorded responses: {"<url>" | "POST <url>": {status,
// headers, file | body | json, rejectHeaders}}, as fixtureFetcher() in
// tools/live_folders/runtime.mjs does.
void ServeResponses(network::TestURLLoaderFactory& factory,
                    const base::DictValue& responses,
                    const base::FilePath& dir) {
  factory.SetInterceptor(base::BindRepeating(
      [](network::TestURLLoaderFactory* factory,
         const base::DictValue* responses, base::FilePath dir,
         const network::ResourceRequest& request) {
        const std::string url = request.url.spec();
        const base::DictValue* entry =
            responses->FindDict(request.method + " " + url);
        if (!entry && request.method == "GET") {
          entry = responses->FindDict(url);
        }
        auto fail = [&] {
          factory->AddResponse(request.url,
                               network::mojom::URLResponseHead::New(), "",
                               network::URLLoaderCompletionStatus(
                                   net::ERR_CONNECTION_REFUSED));
        };
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
        std::string body;
        if (const std::string* file = entry->FindString("file")) {
          base::ScopedAllowBlockingForTesting allow_blocking;
          base::ReadFileToString(dir.AppendASCII(*file), &body);
        } else if (const base::Value* json = entry->Find("json")) {
          body = base::WriteJson(*json).value_or("");
        } else if (const std::string* text = entry->FindString("body")) {
          body = *text;
        }
        std::string raw_headers =
            "HTTP/1.1 " +
            base::NumberToString(entry->FindInt("status").value_or(200)) +
            " OK\n";
        if (const base::DictValue* headers = entry->FindDict("headers")) {
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
        factory->AddResponse(request.url, std::move(head), body,
                             network::URLLoaderCompletionStatus());
      },
      &factory, &responses, dir));
}

class LiveFolderRuntimeBrowserTest : public InProcessBrowserTest {
 protected:
  // The case's outcome as run.mjs prints it: {ok, result | error: {code},
  // fetches: [{method, url, cookies}]}.
  base::Value Run(const Case& c) {
    std::optional<base::Value> spec_value =
        ReadJson(c.dir.AppendASCII("case.json"));
    EXPECT_TRUE(spec_value && spec_value->is_dict()) << c.name;
    const base::DictValue& spec = spec_value->GetDict();

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
    env.Set("now", now)
        .Set("locale", locale ? *locale : "en-US")
        // run.mjs runs in UTC.
        .Set("timeZone", time_zone ? *time_zone : "UTC");

    LiveFetcher::Limits limits;
    base::TimeDelta timeout = base::Seconds(30);
    if (const base::DictValue* l = spec.FindDict("limits")) {
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
    auto run_spec = MakeLiveRunSpec(
        *plugin, config ? *config : empty, secrets,
        spec.FindBool("grantCookies").value_or(false), std::move(env),
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &factory),
        limits, timeout);

    base::DictValue outcome;
    base::ListValue fetches;
    if (!run_spec.has_value()) {
      outcome.Set("ok", false);
      outcome.Set("error",
                  base::DictValue().Set("code", run_spec.error().code));
      outcome.Set("fetches", std::move(fetches));
      return base::Value(std::move(outcome));
    }
    // The fetcher goes with the refresh: its record is taken as it ends.
    LiveFetcher* fetcher = run_spec->fetcher.get();
    std::vector<LiveFetchRecord> records;
    base::test::TestFuture<base::expected<std::string, LiveError>,
                           std::vector<std::string>>
        done;
    LiveFolderService* service = LiveFolderService::Get(browser()->profile());
    EXPECT_TRUE(service);
    service->host().Run(
        std::move(*run_spec),
        base::BindOnce(
            [](LiveFetcher* fetcher, std::vector<LiveFetchRecord>* records,
               base::OnceCallback<void(
                   base::expected<std::string, LiveError>,
                   std::vector<std::string>)> callback,
               base::expected<std::string, LiveError> result,
               std::vector<std::string> log) {
              *records = fetcher->fetches();
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
      fetches.Append(base::DictValue()
                         .Set("method", record.method)
                         .Set("url", record.url.spec())
                         .Set("cookies", record.cookies));
    }
    outcome.Set("fetches", std::move(fetches));
    return base::Value(std::move(outcome));
  }
};

IN_PROC_BROWSER_TEST_F(LiveFolderRuntimeBrowserTest, BuiltinCases) {
  std::vector<Case> cases = BuiltinCases();
  ASSERT_GE(cases.size(), 15u);
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
