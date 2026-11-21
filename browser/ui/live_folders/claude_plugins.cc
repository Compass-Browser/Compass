// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/claude_plugins.h"

#include <optional>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "compass/browser/ui/live_folders/tab_preview_service.h"
#include "compass/browser/ui/webui/sidebar/live_plugin_mojo.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace compass {

namespace {

constexpr char kClaudePrefix[] = "claude.";
constexpr char kTestDir[] = ".claude-test";
constexpr std::string_view kInstalledPrefix = "installed/";

std::string Str(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::string KindOf(const LivePluginManifest& manifest) {
  if (manifest.folder && manifest.preview) {
    return "both";
  }
  return manifest.preview ? "preview" : "folder";
}

std::string SourceOf(const LivePlugin& plugin) {
  if (plugin.builtin) {
    return "builtin";
  }
  if (plugin.local) {
    return plugin.manifest.id.starts_with(kClaudePrefix) ? "claude" : "local";
  }
  return "installed";
}

base::ListValue Strings(const std::vector<std::string>& items) {
  base::ListValue list;
  for (const std::string& item : items) {
    list.Append(item);
  }
  return list;
}

// The folder in <user data>/Compass Plugins a non-built-in plugin's files are
// in, relative to it ("claude.foo/", "com.example/1.0/").
std::string RelativeDir(const LivePlugin& plugin) {
  std::string_view base = plugin.base_path;
  if (base.starts_with(kInstalledPrefix)) {
    base.remove_prefix(kInstalledPrefix.size());
  }
  return std::string(base);
}

// Blocking: a plugin folder's files, {manifest, main_js, icon_svg}, or
// nullopt if it has none.
std::optional<base::DictValue> ReadPluginDir(const base::FilePath& dir) {
  std::string manifest;
  if (!base::ReadFileToString(dir.AppendASCII("manifest.json"), &manifest)) {
    return std::nullopt;
  }
  std::string entry_name = "main.js";
  std::string icon_name;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(manifest, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    if (const std::string* entry = parsed->GetDict().FindString("entry")) {
      entry_name = *entry;
    }
    if (const std::string* icon = parsed->GetDict().FindString("icon")) {
      icon_name = *icon;
    }
  }
  std::string main_js;
  std::string icon_svg;
  base::ReadFileToString(dir.AppendASCII(entry_name), &main_js);
  if (!icon_name.empty()) {
    base::ReadFileToString(dir.AppendASCII(icon_name), &icon_svg);
  }
  return base::DictValue()
      .Set("manifest", std::move(manifest))
      .Set("main_js", std::move(main_js))
      .Set("icon_svg", std::move(icon_svg));
}

// Blocking: replaces |dir| with a plugin folder holding |files|.
bool WritePluginDir(const base::FilePath& dir, const base::DictValue& files) {
  const std::string manifest = Str(files, "manifest");
  std::optional<base::Value> parsed =
      base::JSONReader::Read(manifest, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return false;
  }
  const std::string* entry = parsed->GetDict().FindString("entry");
  const std::string* icon = parsed->GetDict().FindString("icon");
  base::DeletePathRecursively(dir);
  if (!base::CreateDirectory(dir) ||
      !base::WriteFile(dir.AppendASCII("manifest.json"), manifest) ||
      !base::WriteFile(dir.AppendASCII(entry ? *entry : "main.js"),
                       Str(files, "main_js"))) {
    return false;
  }
  if (icon && !Str(files, "icon_svg").empty()) {
    return base::WriteFile(dir.AppendASCII(*icon), Str(files, "icon_svg"));
  }
  return true;
}

LiveFolderService* ServiceFor(const base::WeakPtr<Profile>& profile) {
  return profile ? LiveFolderService::Get(profile.get()) : nullptr;
}

}  // namespace

ClaudePlugins::ClaudePlugins(Profile* profile)
    : profile_(profile->GetWeakPtr()) {}

ClaudePlugins::~ClaudePlugins() = default;

base::ListValue ClaudePlugins::List() {
  base::ListValue list;
  LiveFolderService* service = ServiceFor(profile_);
  if (!service) {
    return list;
  }
  for (const LivePlugin& plugin : service->registry().plugins()) {
    const LivePluginManifest& manifest = plugin.manifest;
    base::DictValue entry = base::DictValue()
                                .Set("id", manifest.id)
                                .Set("name", manifest.name)
                                .Set("kind", KindOf(manifest))
                                .Set("source", SourceOf(plugin));
    if (manifest.preview) {
      entry.Set("matches", Strings(manifest.preview->matches));
      entry.Set("preview_on", service->previews().IsEnabled(plugin));
      if (manifest.preview->session) {
        entry.Set("session_site", manifest.preview->session->site);
        entry.Set("session_granted",
                  service->previews().IsSessionGranted(plugin));
      }
    }
    list.Append(std::move(entry));
  }
  return list;
}

void ClaudePlugins::Get(const std::string& id, DictCallback callback) {
  LiveFolderService* service = ServiceFor(profile_);
  const LivePlugin* plugin = service ? service->registry().Find(id) : nullptr;
  if (!plugin) {
    std::move(callback).Run(base::unexpected(
        "No plugin has the id " + id + ". list_plugins gives the ids."));
    return;
  }
  base::DictValue head = base::DictValue()
                             .Set("id", plugin->manifest.id)
                             .Set("name", plugin->manifest.name)
                             .Set("source", SourceOf(*plugin));
  if (plugin->builtin) {
    head.Set("manifest", LivePluginRegistry::ReadBuiltinFile(
                             plugin->base_path + "manifest.json"));
    head.Set("main_js", LivePluginRegistry::ReadBuiltinFile(
                            plugin->base_path + plugin->manifest.entry));
    std::move(callback).Run(std::move(head));
    return;
  }
  const base::FilePath dir =
      service->plugins_dir().AppendASCII(RelativeDir(*plugin));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()}, base::BindOnce(&ReadPluginDir, dir),
      base::BindOnce(
          [](base::DictValue head, DictCallback callback,
             std::optional<base::DictValue> files) {
            if (!files) {
              std::move(callback).Run(
                  base::unexpected("Its files couldn't be read."));
              return;
            }
            head.Set("manifest", Str(*files, "manifest"));
            head.Set("main_js", Str(*files, "main_js"));
            std::move(callback).Run(std::move(head));
          },
          std::move(head), std::move(callback)));
}

base::expected<base::DictValue, std::string> ClaudePlugins::Check(
    const std::string& manifest_json) {
  std::optional<base::Value> raw =
      base::JSONReader::Read(manifest_json, base::JSON_PARSE_RFC);
  if (!raw || !raw->is_dict()) {
    return base::unexpected("manifest isn't a JSON object");
  }
  base::DictValue& dict = raw->GetDict();
  // Claude's plugins have their own namespace: never a built-in's, nor an
  // installed plugin's.
  std::string id = Str(dict, "id");
  if (id.empty()) {
    return base::unexpected("id is required");
  }
  if (id.starts_with("compass.")) {
    return base::unexpected(
        "compass.* is for built-ins; give your plugin its own id");
  }
  if (!id.starts_with(kClaudePrefix)) {
    id = kClaudePrefix + id;
  }
  dict.Set("id", id);
  auto manifest = ParseLivePluginManifest(*raw);
  if (!manifest.has_value()) {
    return base::unexpected(base::JoinString(manifest.error(), "; "));
  }
  LiveFolderService* service = ServiceFor(profile_);
  const LivePlugin* existing =
      service ? service->registry().Find(manifest->id) : nullptr;
  if (existing && !existing->local) {
    return base::unexpected("An installed plugin has the id " + id +
                            "; pick another");
  }
  base::DictValue out;
  out.Set("manifest",
          base::WriteJsonWithOptions(*raw,
                                     base::JSONWriter::OPTIONS_PRETTY_PRINT)
              .value_or(manifest_json));
  out.Set("id", manifest->id);
  out.Set("name", manifest->name);
  out.Set("kind", KindOf(*manifest));
  out.Set("exists", !!existing);
  out.Set("hosts", Strings(manifest->hosts));
  if (manifest->preview) {
    out.Set("preview_sites", Strings(PreviewSites(*manifest->preview)));
    if (manifest->preview->session) {
      out.Set("session_site", manifest->preview->session->site);
      out.Set("related_hosts", Strings(manifest->preview->session->related));
    }
  }
  return out;
}

void ClaudePlugins::Test(const std::string& manifest_json,
                         const std::string& main_js,
                         bool preview,
                         content::WebContents* tab,
                         base::DictValue config,
                         bool session,
                         DictCallback callback) {
  LiveFolderService* service = ServiceFor(profile_);
  std::optional<base::Value> raw =
      base::JSONReader::Read(manifest_json, base::JSON_PARSE_RFC);
  auto manifest = raw ? ParseLivePluginManifest(*raw)
                      : base::expected<LivePluginManifest, LivePluginErrors>(
                            base::unexpected(LivePluginErrors{"not JSON"}));
  if (!service || !manifest.has_value()) {
    std::move(callback).Run(base::unexpected("The plugin can't run here."));
    return;
  }
  // Served from a scratch folder the plugin scan skips.
  const std::string scratch = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const base::FilePath dir =
      service->plugins_dir().AppendASCII(kTestDir).AppendASCII(scratch);
  LivePlugin plugin;
  plugin.manifest = std::move(*manifest);
  plugin.base_path = base::StrCat({"installed/", kTestDir, "/", scratch, "/"});
  plugin.local = true;
  base::DictValue files = base::DictValue()
                              .Set("manifest", manifest_json)
                              .Set("main_js", main_js);
  auto cleanup = base::BindOnce(
      [](base::FilePath dir) {
        base::ThreadPool::PostTask(
            FROM_HERE, {base::MayBlock()},
            base::BindOnce(base::IgnoreResult(&base::DeletePathRecursively),
                           dir));
      },
      dir);
  auto done = base::BindOnce(
      [](DictCallback callback, base::OnceClosure cleanup,
         base::DictValue result) {
        std::move(cleanup).Run();
        std::move(callback).Run(std::move(result));
      },
      std::move(callback), std::move(cleanup));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&WritePluginDir, dir, std::move(files)),
      base::BindOnce(
          [](base::WeakPtr<ClaudePlugins> self, LivePlugin plugin,
             bool preview, base::WeakPtr<content::WebContents> tab,
             base::DictValue config, bool session,
             base::OnceCallback<void(base::DictValue)> done, bool written) {
            LiveFolderService* service =
                self ? ServiceFor(self->profile_) : nullptr;
            if (!service || !written) {
              std::move(done).Run(base::DictValue().Set("ok", false).Set(
                  "error",
                  base::DictValue()
                      .Set("code", "harness")
                      .Set("message", "The plugin couldn't be written")));
              return;
            }
            if (preview) {
              service->previews().RunTest(std::move(plugin), tab.get(),
                                          std::move(config), session,
                                          std::move(done));
              return;
            }
            // A folder: no cookies, no secrets.
            auto spec = MakeLiveRunSpec(plugin, config, LiveSecrets(),
                                        /*cookies_granted=*/false,
                                        service->MakeEnv(),
                                        service->GetURLLoaderFactory());
            if (!spec.has_value()) {
              std::move(done).Run(base::DictValue().Set("ok", false).Set(
                  "error", base::DictValue()
                               .Set("code", spec.error().code)
                               .Set("message", spec.error().message)));
              return;
            }
            LiveFetchClient* fetcher = spec->fetcher.get();
            service->host().Run(
                std::move(*spec),
                base::BindOnce(
                    [](LiveFetchClient* fetcher,
                       base::OnceCallback<void(base::DictValue)> done,
                       base::expected<std::string, LiveError> result,
                       std::vector<std::string> log) {
                      base::DictValue out;
                      out.Set("ok", result.has_value());
                      if (result.has_value()) {
                        std::optional<base::Value> parsed =
                            base::JSONReader::Read(*result,
                                                   base::JSON_PARSE_RFC);
                        out.Set("result",
                                parsed ? std::move(*parsed) : base::Value());
                      } else {
                        out.Set("error",
                                base::DictValue()
                                    .Set("code", result.error().code)
                                    .Set("message", result.error().message));
                      }
                      base::ListValue lines;
                      for (std::string& line : log) {
                        lines.Append(std::move(line));
                      }
                      out.Set("log", std::move(lines));
                      base::ListValue requests;
                      for (const LiveFetchRecord& record : fetcher->fetches()) {
                        requests.Append(base::DictValue()
                                            .Set("method", record.method)
                                            .Set("url", record.url.spec()));
                      }
                      out.Set("requests", std::move(requests));
                      std::move(done).Run(std::move(out));
                    },
                    fetcher, std::move(done)));
          },
          weak_factory_.GetWeakPtr(), std::move(plugin), preview,
          tab ? tab->GetWeakPtr() : base::WeakPtr<content::WebContents>(),
          std::move(config), session, std::move(done)));
}

void ClaudePlugins::Save(const std::string& manifest_json,
                         const std::string& main_js,
                         const std::string& icon_svg,
                         bool grant_session,
                         ChangeCallback callback) {
  LiveFolderService* service = ServiceFor(profile_);
  std::optional<base::Value> raw =
      base::JSONReader::Read(manifest_json, base::JSON_PARSE_RFC);
  if (!service || !raw || !raw->is_dict()) {
    std::move(callback).Run(base::unexpected("The plugin can't be saved."));
    return;
  }
  // No icon file: no icon (a manifest naming a missing one doesn't load).
  if (icon_svg.empty()) {
    raw->GetDict().Remove("icon");
  }
  auto manifest = ParseLivePluginManifest(*raw);
  if (!manifest.has_value()) {
    std::move(callback).Run(
        base::unexpected(base::JoinString(manifest.error(), "; ")));
    return;
  }
  const std::string id = manifest->id;
  const std::string name = manifest->name;
  const base::FilePath dir = service->plugins_dir().AppendASCII(id);
  base::DictValue files =
      base::DictValue()
          .Set("manifest", base::WriteJsonWithOptions(
                               *raw, base::JSONWriter::OPTIONS_PRETTY_PRINT)
                               .value_or(manifest_json))
          .Set("main_js", main_js)
          .Set("icon_svg", icon_svg);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath dir, base::DictValue files) {
            std::optional<base::DictValue> before = ReadPluginDir(dir);
            const bool written = WritePluginDir(dir, files);
            return std::make_pair(std::move(before), written);
          },
          dir, std::move(files)),
      base::BindOnce(
          [](base::WeakPtr<ClaudePlugins> self, std::string id,
             std::string name, bool grant_session, ChangeCallback callback,
             std::pair<std::optional<base::DictValue>, bool> written) {
            LiveFolderService* service =
                self ? ServiceFor(self->profile_) : nullptr;
            if (!service || !written.second) {
              std::move(callback).Run(
                  base::unexpected("The plugin couldn't be written."));
              return;
            }
            claude::PluginChange change;
            change.kind = written.first ? "updated" : "saved";
            change.id = id;
            change.name = name;
            change.before = std::move(written.first);
            service->RescanPlugins(base::BindOnce(
                [](base::WeakPtr<ClaudePlugins> self, bool grant_session,
                   claude::PluginChange change, ChangeCallback callback) {
                  LiveFolderService* service =
                      self ? ServiceFor(self->profile_) : nullptr;
                  if (!service || !service->registry().Find(change.id)) {
                    std::move(callback).Run(base::unexpected(
                        "Saved, but Compass couldn't load it."));
                    return;
                  }
                  // The user's approval turns it on.
                  service->previews().SetEnabled(change.id, true);
                  if (grant_session) {
                    service->previews().SetSessionGranted(change.id, true);
                  }
                  std::move(callback).Run(std::move(change));
                },
                self, grant_session, std::move(change), std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), id, name, grant_session,
          std::move(callback)));
}

void ClaudePlugins::Delete(const std::string& id, ChangeCallback callback) {
  LiveFolderService* service = ServiceFor(profile_);
  const LivePlugin* plugin = service ? service->registry().Find(id) : nullptr;
  if (!plugin || !plugin->local) {
    std::move(callback).Run(
        base::unexpected("Only local plugins can be deleted here."));
    return;
  }
  const base::FilePath dir =
      service->plugins_dir().AppendASCII(RelativeDir(*plugin));
  claude::PluginChange change;
  change.kind = "deleted";
  change.id = id;
  change.name = plugin->manifest.name;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath dir) {
            std::optional<base::DictValue> before = ReadPluginDir(dir);
            if (before && !base::DeletePathRecursively(dir)) {
              before.reset();
            }
            return before;
          },
          dir),
      base::BindOnce(
          [](base::WeakPtr<ClaudePlugins> self, claude::PluginChange change,
             ChangeCallback callback, std::optional<base::DictValue> before) {
            LiveFolderService* service =
                self ? ServiceFor(self->profile_) : nullptr;
            if (!service || !before) {
              std::move(callback).Run(
                  base::unexpected("The plugin couldn't be deleted."));
              return;
            }
            change.before = std::move(before);
            service->RescanPlugins(base::BindOnce(
                [](claude::PluginChange change, ChangeCallback callback) {
                  std::move(callback).Run(std::move(change));
                },
                std::move(change), std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), std::move(change), std::move(callback)));
}

void ClaudePlugins::Undo(const claude::PluginChange& change,
                         base::OnceCallback<void(bool)> callback) {
  LiveFolderService* service = ServiceFor(profile_);
  if (!service || (change.kind != "saved" && !change.before)) {
    std::move(callback).Run(false);
    return;
  }
  const base::FilePath dir = service->plugins_dir().AppendASCII(change.id);
  std::optional<base::DictValue> before =
      change.before ? std::optional<base::DictValue>(change.before->Clone())
                    : std::nullopt;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath dir, std::optional<base::DictValue> before) {
            // A new plugin goes; a changed or deleted one comes back.
            return before ? WritePluginDir(dir, *before)
                          : base::DeletePathRecursively(dir);
          },
          dir, std::move(before)),
      base::BindOnce(
          [](base::WeakPtr<ClaudePlugins> self,
             base::OnceCallback<void(bool)> callback, bool done) {
            LiveFolderService* service =
                self ? ServiceFor(self->profile_) : nullptr;
            if (!service || !done) {
              std::move(callback).Run(false);
              return;
            }
            service->RescanPlugins(
                base::BindOnce(std::move(callback), true));
          },
          weak_factory_.GetWeakPtr(), std::move(callback)));
}

}  // namespace compass
