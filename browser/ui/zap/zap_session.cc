// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/zap/zap_session.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/boosts/zap.h"
#include "compass/browser/ui/zap/zap_page_js.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/gurl.h"

namespace compass::zap {

namespace {

constexpr base::TimeDelta kPollInterval = base::Milliseconds(150);
constexpr base::TimeDelta kGiveUpAfter = base::Minutes(10);

boosts::BoostStore* StoreFor(content::WebContents* contents) {
  return boosts::BoostStore::Get(
      Profile::FromBrowserContext(contents->GetBrowserContext()));
}

// Runs |call| on zap_page.js's __compassZap in the main frame's isolated
// world, defining it first unless |define| is false.
void RunZapScript(content::WebContents* contents,
                  std::string_view call,
                  base::OnceCallback<void(base::Value)> callback,
                  bool define = true) {
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(base::Value());
    return;
  }
  const std::string script =
      define ? base::StrCat({assets::kZapPageJs, "\n;\n", call})
             : base::StrCat(
                   {"window.__compassZap ? ", call, " : ({state: 'gone'})"});
  frame->ExecuteJavaScriptInIsolatedWorld(base::UTF8ToUTF16(script),
                                          std::move(callback),
                                          ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void ShowToast(content::WebContents* contents, std::string_view text) {
  std::string json;
  base::JSONWriter::Write(base::Value(text), &json);
  RunZapScript(contents, base::StrCat({"__compassZap.toast(", json, ")"}),
               base::DoNothing());
}

}  // namespace

// The bar on one page, from start until Done or cancel.
class ZapSession : public content::WebContentsObserver,
                   public content::WebContentsUserData<ZapSession> {
 public:
  ZapSession(const ZapSession&) = delete;
  ZapSession& operator=(const ZapSession&) = delete;
  ~ZapSession() override = default;

  void Begin(std::string site) {
    site_ = std::move(site);
    poll_timer_.Stop();
    give_up_.Stop();
    GetWebContents().Focus();
    RunZapScript(
        &GetWebContents(), "__compassZap.start()",
        base::BindOnce(&ZapSession::OnStarted, weak_factory_.GetWeakPtr()));
  }

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override { End(); }

 private:
  friend class content::WebContentsUserData<ZapSession>;

  explicit ZapSession(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<ZapSession>(*contents) {}

  void OnStarted(base::Value started) {
    if (!started.is_bool() || !started.GetBool()) {
      End();
      return;
    }
    poll_timer_.Start(
        FROM_HERE, kPollInterval,
        base::BindRepeating(&ZapSession::Poll, base::Unretained(this)));
    give_up_.Start(FROM_HERE, kGiveUpAfter,
                   base::BindOnce(&ZapSession::GiveUp, base::Unretained(this)));
  }

  void Poll() {
    if (polling_) {
      return;
    }
    polling_ = true;
    RunZapScript(
        &GetWebContents(), "__compassZap.poll()",
        base::BindOnce(&ZapSession::OnPolled, weak_factory_.GetWeakPtr()),
        /*define=*/false);
  }

  void OnPolled(base::Value value) {
    polling_ = false;
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      return;
    }
    const std::string* state = dict->FindString("state");
    const base::ListValue* list = dict->FindList("selectors");
    if (!state || *state != "done" || !list || list->empty()) {
      End();
      return;
    }
    std::vector<std::string> selectors;
    for (const base::Value& selector : *list) {
      if (selector.is_string()) {
        selectors.push_back(selector.GetString());
      }
    }
    content::WebContents* contents = &GetWebContents();
    const std::string site = site_;
    End();
    Save(contents, site, selectors);
  }

  static void Save(content::WebContents* contents,
                   const std::string& site,
                   const std::vector<std::string>& selectors) {
    boosts::BoostStore* store = StoreFor(contents);
    if (!store || !store->loaded()) {
      ShowToast(contents, "Zaps can't be kept in this window.");
      return;
    }
    base::expected<boosts::Boost, std::string> saved =
        store->Save(boosts::AddZappedSelectors(
            boosts::FindZapBoost(*store, site), site, selectors));
    if (!saved.has_value()) {
      ShowToast(contents, saved.error());
      return;
    }
    ShowToast(contents,
              base::StrCat({"Hidden on ", site, " from now on (Boost “",
                            saved->name, "”)"}));
  }

  void GiveUp() {
    RunZapScript(&GetWebContents(), "__compassZap.cancel()", base::DoNothing(),
                 /*define=*/false);
    End();
  }

  // Deletes this.
  void End() { GetWebContents().RemoveUserData(UserDataKey()); }

  std::string site_;
  base::RepeatingTimer poll_timer_;
  base::OneShotTimer give_up_;
  bool polling_ = false;
  base::WeakPtrFactory<ZapSession> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(ZapSession);

std::optional<std::string> ZapSiteFor(content::WebContents* contents) {
  if (!contents || !StoreFor(contents)) {
    return std::nullopt;
  }
  const GURL& url = contents->GetLastCommittedURL();
  if (!url.SchemeIsHTTPOrHTTPS() || url.host().empty()) {
    return std::nullopt;
  }
  base::expected<boosts::BoostMatch, std::string> match =
      boosts::BoostMatch::Parse(url.host());
  if (!match.has_value()) {
    return std::nullopt;
  }
  return match->ToString();
}

void StartZap(content::WebContents* contents) {
  std::optional<std::string> site = ZapSiteFor(contents);
  if (!site) {
    return;
  }
  ZapSession::CreateForWebContents(contents);
  ZapSession::FromWebContents(contents)->Begin(std::move(*site));
}

}  // namespace compass::zap
