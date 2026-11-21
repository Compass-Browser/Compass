// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Claude's plugin tools (docs/design/claude.md, "Plugins"): list_plugins,
// get_plugin, test_plugin, save_plugin and delete_plugin, for live folders'
// and tab previews' plugins. They need "Let Claude make plugins"
// (prefs::kPlugins). Saving and deleting ask the user every time, showing
// the code and what the plugin reads and reaches; a test with the tab's
// session asks once per site in a chat. The work is the UI layer's
// (PluginsDelegate, ClaudePlugins).

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/claude/claude_tools.h"
#include "compass/browser/claude/page_reader.h"
#include "content/public/browser/web_contents.h"

namespace compass::claude {

namespace {

constexpr char kPluginsOff[] =
    "Not permitted: the user hasn't turned on \"Let Claude make plugins\" "
    "(Compass settings, Claude). Tell the user if the task needs it.";

// How much code the approval shows.
constexpr size_t kApprovalCodeChars = 12000;

std::string Str(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::string Cut(std::string_view code) {
  std::u16string text = base::UTF8ToUTF16(code);
  if (text.size() <= kApprovalCodeChars) {
    return std::string(code);
  }
  const size_t more = text.size() - kApprovalCodeChars;
  text.resize(kApprovalCodeChars);
  return base::StrCat({base::UTF16ToUTF8(text), "\n… (",
                       base::NumberToString(more), " more characters)"});
}

std::string Joined(const base::DictValue& dict, const char* key) {
  std::vector<std::string> items;
  if (const base::ListValue* list = dict.FindList(key)) {
    for (const base::Value& item : *list) {
      if (item.is_string()) {
        items.push_back(item.GetString());
      }
    }
  }
  return base::JoinString(items, ", ");
}

std::string Json(const base::Value& value) {
  return base::WriteJsonWithOptions(value,
                                    base::JSONWriter::OPTIONS_PRETTY_PRINT)
      .value_or(std::string());
}

// What the approval says the plugin does, from Check()'s answer.
std::string Reach(const base::DictValue& checked) {
  std::string reach;
  if (const std::string sites = Joined(checked, "preview_sites");
      !sites.empty()) {
    base::StrAppend(&reach, {"Reads pages on ", sites,
                             " to show a preview in the tab's hover card.\n"});
  }
  if (const std::string site = Str(checked, "session_site"); !site.empty()) {
    base::StrAppend(&reach, {"Uses your session on ", site,
                             ": it can make requests there as you.\n"});
  }
  if (const std::string related = Joined(checked, "related_hosts");
      !related.empty()) {
    base::StrAppend(&reach,
                    {"Can also reach ", related, ", without your login.\n"});
  }
  if (const std::string hosts = Joined(checked, "hosts"); !hosts.empty()) {
    base::StrAppend(&reach, {"Its live folders reach ", hosts, ".\n"});
  }
  return reach;
}

}  // namespace

void ClaudeTools::RunPluginTool(const std::string& name,
                                base::DictValue input) {
  if (!plugins_) {
    Finish(ToolResult::Error("Plugins aren't available here."));
    return;
  }
  if (!plugins_allowed_ || !plugins_allowed_.Run()) {
    Finish(ToolResult::Error(kPluginsOff));
    return;
  }

  if (name == "list_plugins") {
    base::ListValue plugins = plugins_->List();
    Finish(ToolResult::Text(plugins.empty()
                                ? "No plugins."
                                : Json(base::Value(std::move(plugins)))));
    return;
  }

  if (name == "get_plugin") {
    plugins_->Get(
        Str(input, "id"),
        base::BindOnce(
            [](base::WeakPtr<ClaudeTools> self,
               base::expected<base::DictValue, std::string> files) {
              if (!self) {
                return;
              }
              self->Finish(files.has_value()
                               ? ToolResult::Text(
                                     Json(base::Value(std::move(*files))))
                               : ToolResult::Error(files.error()));
            },
            run_weak_factory_.GetWeakPtr()));
    return;
  }

  if (name == "delete_plugin") {
    const std::string id = Str(input, "id");
    std::string plugin_name;
    std::string source;
    for (const base::Value& plugin : plugins_->List()) {
      if (Str(plugin.GetDict(), "id") == id) {
        plugin_name = Str(plugin.GetDict(), "name");
        source = Str(plugin.GetDict(), "source");
      }
    }
    if (plugin_name.empty()) {
      Finish(ToolResult::Error(base::StrCat(
          {"No plugin has the id ", id, ". list_plugins gives the ids."})));
      return;
    }
    if (source != "claude" && source != "local") {
      Finish(ToolResult::Error(
          "Only plugins you made (or local ones) can be deleted here; the "
          "user removes installed ones in Settings › Plugins."));
      return;
    }
    ApprovalRequest request;
    request.tool = name;
    request.always_ask = true;
    request.title =
        base::StrCat({"Delete the plugin “", plugin_name, "”?"});
    request.detail =
        "Its tab previews stop, and live folders that use it stop "
        "refreshing.";
    ApproveThen(std::move(request),
                base::BindOnce(&ClaudeTools::DeletePlugin,
                               run_weak_factory_.GetWeakPtr(), id));
    return;
  }

  // test_plugin and save_plugin: a plugin Claude wrote, checked first.
  const std::string main_js = Str(input, "main_js");
  base::expected<base::DictValue, std::string> checked =
      plugins_->Check(Str(input, "manifest"));
  if (!checked.has_value()) {
    Finish(ToolResult::Error("The manifest isn't valid: " + checked.error()));
    return;
  }
  if (base::TrimWhitespaceASCII(main_js, base::TRIM_ALL).empty()) {
    Finish(ToolResult::Error("main_js is empty."));
    return;
  }
  std::string manifest = Str(*checked, "manifest");
  const std::string session_site = Str(*checked, "session_site");

  if (name == "test_plugin") {
    const bool preview = Str(input, "mode") != "folder";
    const std::string kind = Str(*checked, "kind");
    if (preview && kind == "folder") {
      Finish(ToolResult::Error("The manifest has no preview section."));
      return;
    }
    if (!preview && kind == "preview") {
      Finish(ToolResult::Error("The manifest says \"folder\": false."));
      return;
    }
    base::DictValue config;
    if (const base::DictValue* given = input.FindDict("config")) {
      config = given->Clone();
    }
    if (!preview || session_site.empty()) {
      TestPlugin(std::move(manifest), main_js, preview, std::move(config),
                 /*session=*/false);
      return;
    }
    // Requests as the user: once per site in a chat ("Allow for this chat"
    // is per tool name).
    ApprovalRequest request;
    request.tool = "test_plugin_session:" + session_site;
    request.title = base::StrCat(
        {"Let Claude test a plugin with your session on ", session_site, "?"});
    request.detail = base::StrCat(
        {"The plugin it's writing makes requests to ", session_site,
         " as you, to read what the page doesn't show. Nothing is saved.\n\n",
         Reach(*checked)});
    ApproveThen(std::move(request),
                base::BindOnce(&ClaudeTools::TestPlugin,
                               run_weak_factory_.GetWeakPtr(),
                               std::move(manifest), main_js, preview,
                               std::move(config), /*session=*/true));
    return;
  }

  // save_plugin: always asks, with the code.
  const bool exists = checked->FindBool("exists").value_or(false);
  ApprovalRequest request;
  request.tool = name;
  request.always_ask = true;
  request.title =
      base::StrCat({exists ? "Change the plugin “" : "Add the plugin “",
                    Str(*checked, "name"), "”?"});
  request.detail =
      base::StrCat({Reach(*checked),
                    session_site.empty()
                        ? ""
                        : "You can turn the session off in Settings › "
                          "Plugins.\n",
                    "\nmain.js:\n\n", Cut(main_js)});
  ApproveThen(std::move(request),
              base::BindOnce(&ClaudeTools::SavePlugin,
                             run_weak_factory_.GetWeakPtr(),
                             std::move(manifest), main_js,
                             Str(input, "icon_svg"), !session_site.empty()));
}

void ClaudeTools::TestPlugin(std::string manifest,
                             std::string main_js,
                             bool preview,
                             base::DictValue config,
                             bool session) {
  content::WebContents* tab = nullptr;
  if (preview) {
    tab = web_contents();
    if (!tab) {
      Finish(ToolResult::Error(
          "A preview test needs a tab: the one you act on is closed."));
      return;
    }
    if (!CanUsePage(tab->GetLastCommittedURL())) {
      Finish(ToolResult::Error(
          "That tab is one of the browser's own pages; test on a web page."));
      return;
    }
  }
  plugins_->Test(
      manifest, main_js, preview, tab, std::move(config), session,
      base::BindOnce(
          [](base::WeakPtr<ClaudeTools> self,
             base::expected<base::DictValue, std::string> result) {
            if (!self) {
              return;
            }
            self->Finish(result.has_value()
                             ? ToolResult::Text(
                                   Json(base::Value(std::move(*result))))
                             : ToolResult::Error(result.error()));
          },
          run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::SavePlugin(std::string manifest,
                             std::string main_js,
                             std::string icon_svg,
                             bool grant_session) {
  plugins_->Save(manifest, main_js, icon_svg, grant_session,
                 base::BindOnce(&ClaudeTools::OnPluginChange,
                                run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::DeletePlugin(std::string id) {
  plugins_->Delete(id, base::BindOnce(&ClaudeTools::OnPluginChange,
                                      run_weak_factory_.GetWeakPtr()));
}

void ClaudeTools::OnPluginChange(
    base::expected<PluginChange, std::string> change) {
  if (!change.has_value()) {
    Finish(ToolResult::Error(change.error()));
    return;
  }
  ToolResult result = ToolResult::Text(Json(base::Value(
      base::DictValue().Set("id", change->id).Set("change", change->kind))));
  result.plugin_change = std::move(*change);
  Finish(std::move(result));
}

}  // namespace compass::claude
