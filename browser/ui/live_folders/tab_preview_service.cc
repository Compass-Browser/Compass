// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/tab_preview_service.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/claude/page_reader.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "compass/browser/ui/live_folders/preview_session_fetcher.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/base/resource/resource_bundle.h"

namespace compass {

namespace {

constexpr std::string_view kSelectScriptPath = "plugins/runtime/select.js";
// How often the browser asks the tab whether a session request is done.
constexpr base::TimeDelta kPollInterval = base::Milliseconds(50);
constexpr int kMaxPolls = 120;

// plugins/runtime/select.js, which installs itself once per document.
const std::u16string& SelectScript() {
  static const base::NoDestructor<std::u16string> script(base::UTF8ToUTF16(
      LivePluginRegistry::ReadBuiltinFile(kSelectScriptPath)));
  return *script;
}

// Calls CompassLive.select.<call> in an isolated world of |contents|' main
// frame, so the page's own scripts can't see or change it.
void RunInTab(content::WebContents* contents,
              const std::string& call,
              base::OnceCallback<void(base::Value)> callback) {
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!frame || !frame->IsRenderFrameLive() || SelectScript().empty()) {
    std::move(callback).Run(base::Value());
    return;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::StrCat({SelectScript(), u"\n;CompassLive.select.",
                    base::UTF8ToUTF16(call), u";"}),
      std::move(callback), ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

bool SameSite(const GURL& a, const GURL& b) {
  const std::string site = LiveSiteOf(a.host());
  return !site.empty() && site == LiveSiteOf(b.host());
}

// Own-site requests from the hovered tab (select.js startFetch() and
// fetchResult(): script results can't wait for a promise, so the browser
// polls).
class TabPageTransport : public PreviewPageTransport {
 public:
  TabPageTransport(base::WeakPtr<content::WebContents> contents, GURL tab_url)
      : contents_(std::move(contents)), tab_url_(std::move(tab_url)) {}
  ~TabPageTransport() override = default;

  void Fetch(const LiveFetchRequest& request,
             size_t max_response_bytes,
             Callback callback) override {
    // The tab may have gone to another site since the snapshot.
    if (!contents_ || !SameSite(contents_->GetLastCommittedURL(), tab_url_)) {
      std::move(callback).Run(base::unexpected(
          LiveError{"network", "The tab went to another page"}));
      return;
    }
    const int id = next_id_++;
    base::DictValue headers;
    for (const auto& [name, value] : request.headers) {
      headers.Set(name, value);
    }
    base::DictValue req;
    req.Set("url", request.url);
    req.Set("method", request.method);
    req.Set("headers", std::move(headers));
    if (request.body) {
      req.Set("body", *request.body);
    }
    req.Set("maxBytes", static_cast<double>(max_response_bytes));
    RunInTab(contents_.get(),
             base::StrCat({"startFetch(", base::NumberToString(id), ",",
                           base::WriteJson(req).value_or("{}"), ")"}),
             base::BindOnce(&TabPageTransport::OnStarted,
                            weak_factory_.GetWeakPtr(), id,
                            std::move(callback)));
  }

 private:
  void OnStarted(int id, Callback callback, base::Value started) {
    if (!started.is_dict()) {
      std::move(callback).Run(base::unexpected(
          LiveError{"network", "The tab couldn't send the request"}));
      return;
    }
    Poll(id, std::move(callback), 0);
  }

  void Poll(int id, Callback callback, int tries) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<TabPageTransport> self, int id, Callback callback,
               int tries) {
              if (!self) {
                return;
              }
              RunInTab(self->contents_.get(),
                       base::StrCat({"fetchResult(", base::NumberToString(id),
                                     ")"}),
                       base::BindOnce(&TabPageTransport::OnPoll, self, id,
                                      std::move(callback), tries));
            },
            weak_factory_.GetWeakPtr(), id, std::move(callback), tries),
        kPollInterval);
  }

  void OnPoll(int id, Callback callback, int tries, base::Value value) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      std::move(callback).Run(base::unexpected(
          LiveError{"network", "The tab stopped answering"}));
      return;
    }
    if (dict->FindBool("pending").value_or(false)) {
      if (tries + 1 >= kMaxPolls) {
        std::move(callback).Run(base::unexpected(
            LiveError{"timeout", "The request took too long"}));
        return;
      }
      Poll(id, std::move(callback), tries + 1);
      return;
    }
    if (const std::string* error = dict->FindString("error")) {
      const std::string* message = dict->FindString("message");
      std::move(callback).Run(base::unexpected(
          LiveError{*error, message ? *message : "The request failed"}));
      return;
    }
    Response response;
    if (dict->FindBool("redirect").value_or(false)) {
      response.redirect = true;
      std::move(callback).Run(std::move(response));
      return;
    }
    response.response.status = dict->FindInt("status").value_or(0);
    const std::string* url = dict->FindString("url");
    response.response.url = GURL(url ? *url : std::string());
    if (const base::DictValue* headers = dict->FindDict("headers")) {
      for (const auto [name, header] : *headers) {
        if (header.is_string()) {
          response.response.headers[base::ToLowerASCII(name)] =
              header.GetString();
        }
      }
    }
    if (const std::string* body = dict->FindString("body")) {
      response.response.body = *body;
    }
    std::move(callback).Run(std::move(response));
  }

  base::WeakPtr<content::WebContents> contents_;
  const GURL tab_url_;
  int next_id_ = 1;
  base::WeakPtrFactory<TabPageTransport> weak_factory_{this};
};

base::DictValue SnapshotOptions(const LivePluginManifest::Preview& preview,
                                bool session_granted) {
  base::DictValue groups;
  for (const auto& [name, group] : preview.select) {
    base::ListValue attrs;
    for (const std::string& attr : group.attrs) {
      attrs.Append(attr);
    }
    base::DictValue fields;
    for (const auto& [key, spec] : group.fields) {
      fields.Set(key, spec);
    }
    groups.Set(name, base::DictValue()
                         .Set("selector", group.selector)
                         .Set("attrs", std::move(attrs))
                         .Set("text", group.text)
                         .Set("fields", std::move(fields))
                         .Set("max", group.max));
  }
  base::ListValue cookies;
  if (session_granted && preview.session) {
    for (const std::string& name : preview.session->page_cookies) {
      cookies.Append(name);
    }
  }
  return base::DictValue()
      .Set("groups", std::move(groups))
      .Set("pageText", preview.page_text)
      .Set("pageCookies", std::move(cookies));
}

}  // namespace

TabPreview::TabPreview() = default;
TabPreview::TabPreview(const TabPreview&) = default;
TabPreview& TabPreview::operator=(const TabPreview&) = default;
TabPreview::~TabPreview() = default;

TabPreviewService::Entry::Entry() = default;
TabPreviewService::Entry::Entry(const Entry&) = default;
TabPreviewService::Entry& TabPreviewService::Entry::operator=(const Entry&) =
    default;
TabPreviewService::Entry::~Entry() = default;

TabPreviewService::TabPreviewService(Profile* profile,
                                     LiveFolderService* service)
    : profile_(profile), service_(service) {
  count_timer_.Start(FROM_HERE, kCountTick,
                     base::BindRepeating(&TabPreviewService::RefreshCounts,
                                         base::Unretained(this)));
}

TabPreviewService::~TabPreviewService() = default;

const LivePlugin* TabPreviewService::FindPlugin(const GURL& url) const {
  if (!profile_->GetPrefs()->GetBoolean(prefs::kTabPreviewsEnabled) ||
      !claude::CanUsePage(url) || url.SchemeIsFile()) {
    return nullptr;
  }
  // Built-ins first, then installed plugins in the registry's order.
  const LivePlugin* found = nullptr;
  for (const LivePlugin& plugin : service_->registry().plugins()) {
    if (!plugin.manifest.preview || !IsEnabled(plugin) ||
        !LivePreviewMatches(*plugin.manifest.preview, url)) {
      continue;
    }
    if (plugin.builtin) {
      return &plugin;
    }
    if (!found) {
      found = &plugin;
    }
  }
  return found;
}

std::optional<TabPreview> TabPreviewService::GetCached(const GURL& url) const {
  auto it = fresh_.find(url.spec());
  if (it == fresh_.end() ||
      base::Time::Now() - it->second.preview.time > it->second.fresh_for) {
    return std::nullopt;
  }
  return it->second.preview;
}

void TabPreviewService::Fetch(content::WebContents* contents,
                              const GURL& url,
                              Callback callback) {
  const LivePlugin* plugin = FindPlugin(url);
  if (!plugin) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  if (std::optional<TabPreview> cached = GetCached(url);
      cached && cached->plugin_id == plugin->manifest.id) {
    std::move(callback).Run(std::move(cached));
    return;
  }
  const std::string key = url.spec();
  std::vector<Callback>& waiting = pending_[key];
  waiting.push_back(std::move(callback));
  if (waiting.size() > 1) {
    return;  // Already running for this page.
  }
  const std::string plugin_id = plugin->manifest.id;
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (frame && frame->IsRenderFrameLive() &&
      contents->GetLastCommittedURL() == url) {
    RunInTab(contents,
             base::StrCat({"snapshot(",
                           base::WriteJson(SnapshotOptions(
                                               *plugin->manifest.preview,
                                               IsSessionGranted(*plugin)))
                               .value_or("{}"),
                           ")"}),
             base::BindOnce(&TabPreviewService::OnSnapshot,
                            weak_factory_.GetWeakPtr(), key, plugin_id,
                            contents->GetWeakPtr()));
    return;
  }
  Run(key, plugin_id, nullptr,
      base::DictValue().Set("url", key).Set("title", "").Set("live", false));
}

void TabPreviewService::OnSnapshot(std::string key,
                                   std::string plugin_id,
                                   base::WeakPtr<content::WebContents> contents,
                                   base::Value snapshot) {
  if (!snapshot.is_dict()) {
    // The page couldn't be read (still loading, say): as an unloaded tab.
    base::DictValue unloaded =
        base::DictValue().Set("url", key).Set("title", "").Set("live", false);
    Run(std::move(key), std::move(plugin_id), nullptr, std::move(unloaded));
    return;
  }
  // The preview is for the page that was hovered.
  snapshot.GetDict().Set("url", key);
  Run(std::move(key), std::move(plugin_id), std::move(contents),
      std::move(snapshot).TakeDict());
}

void TabPreviewService::Run(std::string key,
                            std::string plugin_id,
                            base::WeakPtr<content::WebContents> contents,
                            base::DictValue snapshot) {
  const LivePlugin* plugin = service_->registry().Find(plugin_id);
  if (!plugin || !plugin->manifest.preview) {
    Finish(key, std::nullopt);
    return;
  }
  const bool granted = IsSessionGranted(*plugin);
  const bool live = snapshot.FindBool("live").value_or(false) && contents;
  std::unique_ptr<PreviewPageTransport> transport;
  if (live) {
    transport = std::make_unique<TabPageTransport>(contents, GURL(key));
  }
  auto spec = MakeLivePreviewRunSpec(
      *plugin, snapshot, GetConfig(plugin_id), granted, service_->MakeEnv(),
      service_->GetURLLoaderFactory(), std::move(transport));
  if (!spec.has_value()) {
    OnRun(key, plugin_id, granted, live, base::unexpected(spec.error()), {});
    return;
  }
  service_->host().Run(
      std::move(*spec),
      base::BindOnce(&TabPreviewService::OnRun, weak_factory_.GetWeakPtr(),
                     key, plugin_id, granted, live));
}

void TabPreviewService::OnRun(std::string key,
                              std::string plugin_id,
                              bool session,
                              bool live,
                              base::expected<std::string, LiveError> result,
                              std::vector<std::string> log) {
  if (!result.has_value()) {
    log.push_back(base::StrCat(
        {"Preview failed: ", result.error().code, ": ", result.error().message}));
  }
  logs_[plugin_id] = std::move(log);
  const LivePlugin* plugin = service_->registry().Find(plugin_id);
  std::optional<TabPreview> preview;
  if (plugin && plugin->manifest.preview && result.has_value()) {
    const GURL tab_url(key);
    const LivePluginManifest::Preview& spec = *plugin->manifest.preview;
    auto parsed = ParseLivePreviewResult(*result, [&](const GURL& icon) {
      return PreviewIconAllowed(spec, tab_url, session, icon);
    });
    // The tab row's badge, whether or not there's anything to show in the
    // card.
    if (parsed.has_value() && spec.badge) {
      TabCount count{parsed->count, parsed->some};
      TabCount& kept = counts_[key];
      if (kept != count) {
        kept = count;
        observers_.Notify(&Observer::OnTabCountsChanged);
      }
    }
    if (parsed.has_value() && !parsed->empty()) {
      preview.emplace();
      preview->plugin_id = plugin_id;
      preview->plugin_name = base::UTF8ToUTF16(plugin->manifest.name);
      preview->result = std::move(*parsed);
      preview->time = base::Time::Now();
      Entry entry;
      entry.preview = *preview;
      entry.fresh_for = session ? kSessionFreshFor : kFreshFor;
      fresh_[key] = std::move(entry);
      if (live) {
        last_good_[key] = *preview;
      }
    }
  }
  // An unloaded tab that couldn't say anything: what it showed last time.
  if (!preview && !live) {
    if (auto it = last_good_.find(key); it != last_good_.end()) {
      preview = it->second;
      preview->stale = true;
      preview->result.footer =
          u"Last seen " + base::TimeFormatTimeOfDay(preview->time);
    }
  }
  Finish(key, std::move(preview));
}

void TabPreviewService::Finish(const std::string& key,
                               std::optional<TabPreview> preview) {
  auto it = pending_.find(key);
  if (it == pending_.end()) {
    return;
  }
  std::vector<Callback> callbacks = std::move(it->second);
  pending_.erase(it);
  for (Callback& callback : callbacks) {
    std::move(callback).Run(preview);
  }
}

const base::DictValue* TabPreviewService::PluginPrefs(
    const std::string& plugin_id) const {
  return profile_->GetPrefs()
      ->GetDict(prefs::kTabPreviewPlugins)
      .FindDict(plugin_id);
}

void TabPreviewService::UpdatePluginPrefs(
    const std::string& plugin_id,
    base::OnceCallback<void(base::DictValue&)> update) {
  ScopedDictPrefUpdate prefs_update(profile_->GetPrefs(),
                                    prefs::kTabPreviewPlugins);
  base::DictValue* dict = prefs_update->EnsureDict(plugin_id);
  std::move(update).Run(*dict);
  ClearCache();
}

bool TabPreviewService::IsEnabled(const LivePlugin& plugin) const {
  const base::DictValue* dict = PluginPrefs(plugin.manifest.id);
  std::optional<bool> enabled =
      dict ? dict->FindBool("enabled") : std::nullopt;
  // Built-ins are on by default; installed plugins wait for the user.
  return enabled.value_or(plugin.builtin);
}

void TabPreviewService::SetEnabled(const std::string& plugin_id,
                                   bool enabled) {
  UpdatePluginPrefs(plugin_id,
                    base::BindOnce([](bool enabled, base::DictValue& dict) {
                      dict.Set("enabled", enabled);
                    }, enabled));
}

bool TabPreviewService::IsSessionGranted(const LivePlugin& plugin) const {
  if (!plugin.manifest.preview || !plugin.manifest.preview->session) {
    return false;
  }
  const base::DictValue* dict = PluginPrefs(plugin.manifest.id);
  const std::optional<bool> granted =
      dict ? dict->FindBool("session_granted") : std::nullopt;
  // Built-ins ship with Compass, related hosts and all: their session is on
  // until the user turns it off. Installed plugins wait for the user.
  if (plugin.builtin) {
    return granted.value_or(true);
  }
  if (!granted.value_or(false)) {
    return false;
  }
  // Granted for the related hosts the user saw: an update that lists more
  // asks again.
  const base::ListValue* granted_hosts = dict->FindList("session_hosts");
  for (const std::string& host : plugin.manifest.preview->session->related) {
    if (!granted_hosts ||
        !std::ranges::contains(*granted_hosts, base::Value(host))) {
      return false;
    }
  }
  return true;
}

void TabPreviewService::SetSessionGranted(const std::string& plugin_id,
                                          bool granted) {
  base::ListValue hosts;
  if (const LivePlugin* plugin = service_->registry().Find(plugin_id);
      plugin && plugin->manifest.preview && plugin->manifest.preview->session) {
    for (const std::string& host : plugin->manifest.preview->session->related) {
      hosts.Append(host);
    }
  }
  UpdatePluginPrefs(
      plugin_id,
      base::BindOnce(
          [](bool granted, base::ListValue hosts, base::DictValue& dict) {
            dict.Set("session_granted", granted);
            dict.Set("session_hosts", std::move(hosts));
          },
          granted, std::move(hosts)));
  // A revoked session leaves nothing behind that it fetched.
  if (!granted) {
    last_good_.clear();
  }
}

base::DictValue TabPreviewService::GetConfig(
    const std::string& plugin_id) const {
  const base::DictValue* dict = PluginPrefs(plugin_id);
  const base::DictValue* config = dict ? dict->FindDict("config") : nullptr;
  return config ? config->Clone() : base::DictValue();
}

void TabPreviewService::SetConfig(const std::string& plugin_id,
                                  base::DictValue config) {
  UpdatePluginPrefs(plugin_id, base::BindOnce(
                                   [](base::DictValue config,
                                      base::DictValue& dict) {
                                     dict.Set("config", std::move(config));
                                   },
                                   std::move(config)));
}

void TabPreviewService::ClearCache() {
  fresh_.clear();
  // Settings changed: badges are counted again, soon.
  counted_.clear();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabPreviewService::RefreshCounts,
                                weak_factory_.GetWeakPtr()));
}

TabCount TabPreviewService::GetCount(const GURL& url) const {
  auto it = counts_.find(url.spec());
  return it == counts_.end() ? TabCount() : it->second;
}

void TabPreviewService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void TabPreviewService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void TabPreviewService::RefreshCounts() {
  const base::Time now = base::Time::Now();
  std::set<std::string> open;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_) {
          return true;
        }
        TabStripModel* strip = browser->GetTabStripModel();
        for (int i = 0; strip && i < strip->count(); ++i) {
          content::WebContents* contents = strip->GetWebContentsAt(i);
          const GURL& url = contents->GetLastCommittedURL();
          const LivePlugin* plugin = FindPlugin(url);
          if (!plugin || !plugin->manifest.preview->badge) {
            continue;
          }
          const std::string key = url.spec();
          open.insert(key);
          // A discarded tab keeps its last count; it has no page to read.
          if (contents->WasDiscarded()) {
            continue;
          }
          const base::TimeDelta every =
              IsSessionGranted(*plugin) ? kSessionCountEvery : kCountEvery;
          auto it = counted_.find(key);
          if (it != counted_.end() && now - it->second < every) {
            continue;
          }
          counted_[key] = now;
          Fetch(contents, url, base::DoNothing());
        }
        return true;
      });
  // Pages no longer open (or no longer matched) lose their badges.
  const size_t before = counts_.size();
  std::erase_if(counts_, [&](const auto& entry) {
    return !open.contains(entry.first);
  });
  std::erase_if(counted_, [&](const auto& entry) {
    return !open.contains(entry.first);
  });
  if (counts_.size() != before) {
    observers_.Notify(&Observer::OnTabCountsChanged);
  }
}

std::vector<std::string> TabPreviewService::GetLog(
    const std::string& plugin_id) const {
  auto it = logs_.find(plugin_id);
  return it == logs_.end() ? std::vector<std::string>() : it->second;
}

void TabPreviewService::RunTest(
    LivePlugin plugin,
    content::WebContents* contents,
    base::DictValue config,
    bool session,
    base::OnceCallback<void(base::DictValue)> callback) {
  if (!plugin.manifest.preview || !contents) {
    std::move(callback).Run(base::DictValue().Set("ok", false).Set(
        "error", base::DictValue()
                     .Set("code", "bad_plugin")
                     .Set("message", "No preview, or no tab")));
    return;
  }
  const std::string options =
      base::WriteJson(SnapshotOptions(*plugin.manifest.preview, session))
          .value_or("{}");
  RunInTab(contents, base::StrCat({"snapshot(", options, ")"}),
           base::BindOnce(&TabPreviewService::OnTestSnapshot,
                          weak_factory_.GetWeakPtr(), std::move(plugin),
                          contents->GetWeakPtr(), std::move(config), session,
                          std::move(callback)));
}

void TabPreviewService::OnTestSnapshot(
    LivePlugin plugin,
    base::WeakPtr<content::WebContents> contents,
    base::DictValue config,
    bool session,
    base::OnceCallback<void(base::DictValue)> callback,
    base::Value snapshot) {
  const bool live = snapshot.is_dict() && contents;
  base::DictValue page =
      live ? std::move(snapshot).TakeDict()
           : base::DictValue().Set("title", "").Set("live", false);
  if (contents) {
    page.Set("url", contents->GetLastCommittedURL().spec());
  }
  // What previewTab() got, briefly, so selectors can be fixed.
  std::string excerpt = base::WriteJson(page).value_or("");
  if (excerpt.size() > 4000) {
    base::TruncateUTF8ToByteSize(excerpt, 4000, &excerpt);
    excerpt += "…";
  }
  std::unique_ptr<PreviewPageTransport> transport;
  if (live) {
    transport = std::make_unique<TabPageTransport>(
        contents, contents->GetLastCommittedURL());
  }
  auto spec = MakeLivePreviewRunSpec(plugin, page, config, session,
                                     service_->MakeEnv(),
                                     service_->GetURLLoaderFactory(),
                                     std::move(transport));
  if (!spec.has_value()) {
    std::move(callback).Run(
        base::DictValue()
            .Set("ok", false)
            .Set("error", base::DictValue()
                              .Set("code", spec.error().code)
                              .Set("message", spec.error().message))
            .Set("snapshot", excerpt));
    return;
  }
  LiveFetchClient* fetcher = spec->fetcher.get();
  service_->host().Run(
      std::move(*spec),
      base::BindOnce(
          [](LiveFetchClient* fetcher, std::string excerpt,
             base::OnceCallback<void(base::DictValue)> callback,
             base::expected<std::string, LiveError> result,
             std::vector<std::string> log) {
            base::DictValue out;
            out.Set("ok", result.has_value());
            if (result.has_value()) {
              std::optional<base::Value> parsed =
                  base::JSONReader::Read(*result, base::JSON_PARSE_RFC);
              out.Set("result", parsed ? std::move(*parsed) : base::Value());
            } else {
              out.Set("error", base::DictValue()
                                   .Set("code", result.error().code)
                                   .Set("message", result.error().message));
            }
            base::ListValue lines;
            for (std::string& line : log) {
              lines.Append(std::move(line));
            }
            out.Set("log", std::move(lines));
            base::ListValue requests;
            if (fetcher) {
              for (const LiveFetchRecord& record : fetcher->fetches()) {
                requests.Append(base::DictValue()
                                    .Set("method", record.method)
                                    .Set("url", record.url.spec())
                                    .Set("cookies", record.cookies)
                                    .Set("via", record.via)
                                    .Set("redirect", record.redirect));
              }
            }
            out.Set("requests", std::move(requests));
            out.Set("snapshot", std::move(excerpt));
            std::move(callback).Run(std::move(out));
          },
          fetcher, std::move(excerpt), std::move(callback)));
}

}  // namespace compass
