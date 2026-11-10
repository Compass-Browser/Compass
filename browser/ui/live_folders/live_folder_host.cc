// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/live_folder_host.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_ANDROID)
#include "compass/browser/ui/live_folders/live_folders_untrusted_ui.h"
#endif

namespace compass {

namespace {

// What a plugin may log per refresh.
constexpr size_t kMaxLogLines = 100;
constexpr size_t kMaxLogLineLength = 1000;

// The page enforces the refresh's timeout; this one catches a page that
// doesn't (a hung or compromised renderer).
constexpr base::TimeDelta kBackstopSlack = base::Seconds(5);

live_folders::mojom::FetchResultPtr FetchError(std::string code,
                                               std::string message) {
  auto result = live_folders::mojom::FetchResult::New();
  result->ok = false;
  result->error_code = std::move(code);
  result->error_message = std::move(message);
  return result;
}

}  // namespace

LiveFolderHost::RunSpec::RunSpec() = default;
LiveFolderHost::RunSpec::RunSpec(RunSpec&&) = default;
LiveFolderHost::RunSpec& LiveFolderHost::RunSpec::operator=(RunSpec&&) =
    default;
LiveFolderHost::RunSpec::~RunSpec() = default;

struct LiveFolderHost::Running {
  RunSpec spec;
  RunCallback callback;
  std::vector<std::string> log;
  bool started = false;
  base::OneShotTimer backstop;
};

LiveFolderHost::LiveFolderHost(Profile* profile) : profile_(profile) {}

LiveFolderHost::~LiveFolderHost() = default;

int LiveFolderHost::Run(RunSpec spec, RunCallback callback) {
  const int run_id = next_run_id_++;
  auto running = std::make_unique<Running>();
  running->spec = std::move(spec);
  running->callback = std::move(callback);
  running->backstop.Start(
      FROM_HERE, running->spec.timeout + kBackstopSlack,
      base::BindOnce(&LiveFolderHost::Finish, base::Unretained(this), run_id,
                     base::unexpected(LiveError{
                         "timeout", "The refresh took too long"})));
  runs_[run_id] = std::move(running);
  if (page_.is_bound()) {
    StartOnPage(run_id);
  } else {
    EnsurePage();
  }
  return run_id;
}

void LiveFolderHost::Cancel(int run_id) {
  if (runs_.erase(run_id) && page_.is_bound()) {
    page_->Cancel(run_id);
  }
}

bool LiveFolderHost::IsHostContents(
    const content::WebContents* contents) const {
  return contents && contents == web_contents_.get();
}

void LiveFolderHost::BindPage(
    mojo::PendingRemote<live_folders::mojom::Page> page,
    mojo::PendingReceiver<live_folders::mojom::PageHandler> handler) {
  page_.reset();
  receiver_.reset();
  page_.Bind(std::move(page));
  receiver_.Bind(std::move(handler));
  page_.set_disconnect_handler(base::BindOnce(&LiveFolderHost::OnPageGone,
                                              base::Unretained(this)));
  // Refreshes that waited for the page.
  std::vector<int> waiting;
  for (const auto& [run_id, running] : runs_) {
    if (!running->started) {
      waiting.push_back(run_id);
    }
  }
  for (int run_id : waiting) {
    StartOnPage(run_id);
  }
}

void LiveFolderHost::EnsurePage() {
  if (web_contents_) {
    return;
  }
#if BUILDFLAG(IS_ANDROID)
  RegisterLiveFoldersUntrustedUI();
#else
  // The page's Mojo binder is added to the untrusted registry while Compass
  // fills the trusted one (compass_web_ui_registry.cc), which happens on
  // first use: make sure it has.
  content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry();
#endif
  const GURL url(kLiveFoldersUrl);
  content::WebContents::CreateParams params(
      profile_, content::SiteInstance::CreateForURL(profile_, url));
  web_contents_ = content::WebContents::Create(params);
  content::NavigationController::LoadURLParams load(url);
  load.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  web_contents_->GetController().LoadURLWithParams(load);
}

void LiveFolderHost::StartOnPage(int run_id) {
  auto it = runs_.find(run_id);
  if (it == runs_.end()) {
    return;
  }
  Running& running = *it->second;
  running.started = true;
  auto request = live_folders::mojom::RunRequest::New();
  request->run_id = run_id;
  request->scripts = running.spec.scripts;
  request->config_json = base::WriteJson(running.spec.config).value_or("{}");
  request->env_json = base::WriteJson(running.spec.env).value_or("{}");
  request->timeout_ms =
      static_cast<int32_t>(running.spec.timeout.InMilliseconds());
  request->max_items = running.spec.max_items;
  page_->Run(std::move(request),
             base::BindOnce(&LiveFolderHost::OnRunDone,
                            weak_factory_.GetWeakPtr(), run_id));
}

void LiveFolderHost::OnRunDone(int run_id,
                               live_folders::mojom::RunResultPtr result) {
  if (result->ok) {
    Finish(run_id, std::move(result->result_json));
  } else {
    Finish(run_id, base::unexpected(LiveError{result->error_code,
                                              result->error_message}));
  }
}

void LiveFolderHost::Finish(int run_id,
                            base::expected<std::string, LiveError> result) {
  auto it = runs_.find(run_id);
  if (it == runs_.end()) {
    return;
  }
  std::unique_ptr<Running> running = std::move(it->second);
  runs_.erase(it);
  if (!result.has_value() && result.error().code == "timeout" &&
      page_.is_bound()) {
    page_->Cancel(run_id);
  }
  std::move(running->callback)
      .Run(std::move(result), std::move(running->log));
}

void LiveFolderHost::OnPageGone() {
  page_.reset();
  receiver_.reset();
  std::vector<int> started;
  for (const auto& [run_id, running] : runs_) {
    if (running->started) {
      started.push_back(run_id);
    }
  }
  for (int run_id : started) {
    Finish(run_id, base::unexpected(LiveError{
                       "plugin_error", "The plugin's page stopped"}));
  }
  // Not from inside the disconnect handler: the page's WebContents may be
  // the one calling it.
  if (web_contents_) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(web_contents_));
  }
  if (!runs_.empty()) {
    EnsurePage();
  }
}

void LiveFolderHost::Fetch(int32_t run_id,
                           live_folders::mojom::FetchRequestPtr request,
                           FetchCallback callback) {
  // Answer even if the refresh ends first: the page is waiting.
  callback = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      std::move(callback), FetchError("network", "The refresh ended"));
  auto it = runs_.find(run_id);
  if (it == runs_.end() || !it->second->spec.fetcher) {
    std::move(callback).Run(FetchError("network", "No such refresh"));
    return;
  }
  LiveFetchRequest fetch;
  fetch.url = request->url;
  fetch.method = request->method;
  for (auto& [name, value] : request->headers) {
    fetch.headers.emplace_back(name, value);
  }
  fetch.body = request->body;
  fetch.include_credentials = request->include_credentials;
  it->second->spec.fetcher->Fetch(
      fetch, base::BindOnce(
                 [](FetchCallback callback,
                    base::expected<LiveFetchResponse, LiveError> response) {
                   if (!response.has_value()) {
                     std::move(callback).Run(
                         FetchError(response.error().code,
                                    response.error().message));
                     return;
                   }
                   auto result = live_folders::mojom::FetchResult::New();
                   result->ok = true;
                   result->status = response->status;
                   result->url = response->url.spec();
                   result->headers = base::flat_map<std::string, std::string>(
                       response->headers.begin(), response->headers.end());
                   result->body = std::move(response->body);
                   std::move(callback).Run(std::move(result));
                 },
                 std::move(callback)));
}

void LiveFolderHost::Log(int32_t run_id, const std::string& message) {
  auto it = runs_.find(run_id);
  if (it == runs_.end() || it->second->log.size() >= kMaxLogLines) {
    return;
  }
  it->second->log.push_back(message.substr(0, kMaxLogLineLength));
}

}  // namespace compass
