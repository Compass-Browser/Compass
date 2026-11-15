// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Claude's Boost tools (docs/design/boosts.md, "Claude"): list_boosts,
// save_boost and delete_boost change the profile's Boosts. They need "Let
// Claude make Boosts" (prefs::kBoosts). A change that leaves a Boost with
// JavaScript to run (new code, another world, other sites, turned back on)
// and deleting one with JavaScript ask the user every time; CSS-only changes
// save at once, and the chat shows them with an Undo.

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/claude/claude_tools.h"

namespace compass::claude {

namespace {

constexpr char kBoostsOff[] =
    "Not permitted: the user hasn't turned on \"Let Claude make Boosts\" "
    "(Compass settings, Claude). Tell the user if the task needs it; they "
    "can also make Boosts by hand in Compass settings, Boosts.";

// How much of a Boost's JavaScript the approval shows: all of it, unless
// it's long.
constexpr size_t kApprovalJsChars = 12000;
// list_boosts without a host lists code only up to this size, in all.
constexpr size_t kMaxListedCode = 60000;

std::string Str(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

bool IsBlank(std::string_view code) {
  return base::TrimWhitespaceASCII(code, base::TRIM_ALL).empty();
}

std::string Cut(std::string_view code) {
  std::u16string text = base::UTF8ToUTF16(code);
  if (text.size() <= kApprovalJsChars) {
    return std::string(code);
  }
  const size_t more = text.size() - kApprovalJsChars;
  text.resize(kApprovalJsChars);
  return base::StrCat({base::UTF16ToUTF8(text), "\n… (",
                       base::NumberToString(more), " more characters)"});
}

std::string WorldName(const boosts::Boost& boost) {
  return boost.js_world == boosts::kMainWorld ? "the page's own JavaScript"
                                              : "an isolated world";
}

// Whether saving |boost| over |before| (none: new) gives JavaScript to run
// that the user hasn't allowed: new or other code, another world, other
// sites, or a disabled Boost turned on. Taking JavaScript out never asks.
bool NeedsApproval(const boosts::Boost& boost,
                   const std::optional<boosts::Boost>& before) {
  if (IsBlank(boost.js) || !boost.enabled) {
    return false;
  }
  return !before || IsBlank(before->js) || before->js != boost.js ||
         before->js_world != boost.js_world || before->match != boost.match ||
         !before->enabled;
}

base::DictValue Listed(const boosts::Boost& boost, bool with_code) {
  base::DictValue dict = base::DictValue()
                             .Set("id", boost.id)
                             .Set("name", boost.name)
                             .Set("match", boost.match)
                             .Set("enabled", boost.enabled)
                             .Set("author", boost.author)
                             .Set("js_world", boost.js_world);
  if (with_code) {
    dict.Set("css", boost.css).Set("js", boost.js);
  } else {
    dict.Set("css_chars", static_cast<int>(boost.css.size()))
        .Set("js_chars", static_cast<int>(boost.js.size()));
  }
  return dict;
}

std::string Json(const base::Value& value) {
  return base::WriteJsonWithOptions(value,
                                    base::JSONWriter::OPTIONS_PRETTY_PRINT)
      .value_or(std::string());
}

}  // namespace

void ClaudeTools::SetBoosts(
    boosts::BoostStore* store,
    base::RepeatingCallback<bool()> allowed,
    base::RepeatingCallback<void(base::OnceClosure)> when_applied) {
  boosts_ = store ? store->GetWeakPtr() : nullptr;
  boosts_allowed_ = std::move(allowed);
  boosts_applied_ = std::move(when_applied);
}

void ClaudeTools::RunBoostTool(const std::string& name,
                               base::DictValue input) {
  if (!boosts_) {
    Finish(ToolResult::Error("Boosts aren't available here."));
    return;
  }
  if (!boosts_allowed_ || !boosts_allowed_.Run()) {
    Finish(ToolResult::Error(kBoostsOff));
    return;
  }
  if (!boosts_->loaded()) {
    Finish(ToolResult::Error("Boosts are still loading. Try again in a "
                             "moment."));
    return;
  }

  if (name == "list_boosts") {
    const std::string host = base::ToLowerASCII(
        base::TrimWhitespaceASCII(Str(input, "host"), base::TRIM_ALL));
    base::ListValue listed;
    size_t code = 0;
    for (const boosts::Boost& boost : boosts_->boosts()) {
      code += boost.css.size() + boost.js.size();
    }
    for (const boosts::Boost& boost : boosts_->boosts()) {
      if (!host.empty()) {
        base::expected<boosts::BoostMatch, std::string> match =
            boosts::BoostMatch::Parse(boost.match);
        if (!match.has_value() || !match->MatchesHost(host)) {
          continue;
        }
      }
      // A site's Boosts come with their code; all of them, only while
      // that's short.
      listed.Append(Listed(boost, !host.empty() || code <= kMaxListedCode));
    }
    if (listed.empty()) {
      Finish(ToolResult::Text(host.empty() ? "No Boosts yet."
                                           : "No Boosts for " + host + "."));
      return;
    }
    Finish(ToolResult::Text(Json(base::Value(std::move(listed)))));
    return;
  }

  if (name == "delete_boost") {
    const boosts::Boost* existing = boosts_->Find(Str(input, "id"));
    if (!existing) {
      Finish(ToolResult::Error(base::StrCat(
          {"No Boost has the id ", Str(input, "id"),
           ". list_boosts gives the ids."})));
      return;
    }
    boosts::Boost before = *existing;
    if (IsBlank(before.js)) {
      DeleteBoost(std::move(before));
      return;
    }
    ApprovalRequest request;
    request.tool = name;
    request.always_ask = true;
    request.title = base::StrCat({"Delete the Boost “", before.name, "” for ",
                                  before.match, "?"});
    request.detail = base::StrCat(
        {"It runs this JavaScript in ", WorldName(before), ":\n\n",
         Cut(before.js)});
    ApproveThen(std::move(request),
                base::BindOnce(&ClaudeTools::DeleteBoost,
                               run_weak_factory_.GetWeakPtr(),
                               std::move(before)));
    return;
  }

  // save_boost: the Boost as it'll be, from the one it updates.
  boosts::Boost boost;
  std::optional<boosts::Boost> before;
  const std::string id = Str(input, "id");
  if (!id.empty()) {
    const boosts::Boost* existing = boosts_->Find(id);
    if (!existing) {
      Finish(ToolResult::Error(base::StrCat(
          {"No Boost has the id ", id,
           ". list_boosts gives the ids; leave id out to make a new one."})));
      return;
    }
    before = *existing;
    boost = *existing;
  } else {
    boost.author = boosts::kAuthorClaude;
  }
  if (const std::string* value = input.FindString("name")) {
    boost.name = *value;
  }
  if (const std::string* value = input.FindString("match")) {
    boost.match = *value;
  }
  if (const std::string* value = input.FindString("css")) {
    boost.css = *value;
  }
  if (const std::string* value = input.FindString("js")) {
    boost.js = *value;
  }
  if (const std::string* value = input.FindString("js_world")) {
    boost.js_world = *value;
  }
  if (std::optional<bool> enabled = input.FindBool("enabled")) {
    boost.enabled = *enabled;
  }
  // Refused before the user is asked.
  base::expected<boosts::Boost, std::string> normalized =
      boosts::NormalizeBoost(boost);
  if (!normalized.has_value()) {
    Finish(ToolResult::Error(normalized.error()));
    return;
  }
  boost = std::move(*normalized);
  if (!NeedsApproval(boost, before)) {
    SaveBoost(std::move(boost), std::move(before));
    return;
  }
  ApprovalRequest request;
  request.tool = name;
  request.always_ask = true;
  request.title =
      base::StrCat({before ? "Change the Boost “" : "Add the Boost “",
                    boost.name, "” for ", boost.match, ", with JavaScript?"});
  std::string detail =
      base::StrCat({"Runs on ", boost.match, " in ", WorldName(boost), ":\n\n",
                    Cut(boost.js)});
  if (before && !IsBlank(before->js) && before->js != boost.js) {
    base::StrAppend(&detail, {"\n\nIt replaces:\n\n", Cut(before->js)});
  }
  request.detail = std::move(detail);
  ApproveThen(std::move(request),
              base::BindOnce(&ClaudeTools::SaveBoost,
                             run_weak_factory_.GetWeakPtr(), std::move(boost),
                             std::move(before)));
}

void ClaudeTools::SaveBoost(boosts::Boost boost,
                            std::optional<boosts::Boost> before) {
  if (!boosts_) {
    Finish(ToolResult::Error("Boosts aren't available here."));
    return;
  }
  base::expected<boosts::Boost, std::string> saved =
      boosts_->Save(std::move(boost));
  if (!saved.has_value()) {
    Finish(ToolResult::Error(saved.error()));
    return;
  }
  BoostChange change;
  change.kind = before ? "updated" : "saved";
  change.id = saved->id;
  change.match = saved->match;
  change.before = std::move(before);
  // The same shape on iOS: Claude reloads the tab (navigate) to see it.
  std::string text = Json(base::Value(base::DictValue()
                                          .Set("id", saved->id)
                                          .Set("needs_reload", true)));
  FinishBoostChange(std::move(change), std::move(text));
}

void ClaudeTools::DeleteBoost(boosts::Boost before) {
  if (!boosts_ || !boosts_->Remove(before.id)) {
    Finish(ToolResult::Error("That Boost is already gone."));
    return;
  }
  BoostChange change;
  change.kind = "deleted";
  change.id = before.id;
  change.match = before.match;
  change.before = std::move(before);
  FinishBoostChange(std::move(change),
                    "Deleted. Pages loaded from now on don't get it.");
}

void ClaudeTools::FinishBoostChange(BoostChange change, std::string text) {
  base::OnceClosure finish = base::BindOnce(
      [](base::WeakPtr<ClaudeTools> self, BoostChange change,
         std::string text) {
        if (!self) {
          return;
        }
        ToolResult result = ToolResult::Text(std::move(text));
        result.boost_change = std::move(change);
        self->Finish(std::move(result));
      },
      run_weak_factory_.GetWeakPtr(), std::move(change), std::move(text));
  if (boosts_applied_) {
    // So a reload right after gets it.
    boosts_applied_.Run(std::move(finish));
    return;
  }
  std::move(finish).Run();
}

}  // namespace compass::claude
