// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_capture.h"

#include <string>
#include <utility>

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
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/easels/easel_capture_core.h"
#include "compass/browser/ui/easels/easel_context_menu.h"
#include "compass/browser/ui/easels/easel_geometry.h"
#include "compass/browser/ui/easels/easel_titles.h"
#include "compass/browser/ui/easels/page_screenshot.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/page.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/menus/simple_menu_model.h"
#include "url/gurl.h"

namespace compass::easels {

namespace {

// How often the page is asked whether the user is done.
constexpr base::TimeDelta kPollInterval = base::Milliseconds(120);
// An overlay left up this long is taken down.
constexpr base::TimeDelta kGiveUpAfter = base::Minutes(10);

std::string Quote(std::string_view text) {
  std::string json;
  base::JSONWriter::Write(base::Value(text), &json);
  return json;
}

void ShowToast(content::WebContents* contents, std::string_view text) {
  RunPageScript(contents, base::StrCat({"__compassEasel.toast(", Quote(text),
                                        ")"}),
                base::DoNothing());
}

BrowserWindowInterface* BrowserOf(content::WebContents* contents) {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  return tab ? tab->GetBrowserWindowInterface() : nullptr;
}

// The capture is on the board (or isn't): say so on the page, and give a
// new board its tab.
void OnAdded(base::WeakPtr<content::WebContents> contents,
             bool new_easel,
             base::expected<sync_pb::CompassEaselObject, std::string> saved) {
  if (!contents) {
    return;
  }
  if (!saved.has_value()) {
    ShowToast(contents.get(), saved.error());
    return;
  }
  Profile* profile =
      Profile::FromBrowserContext(contents->GetBrowserContext());
  EaselStore* store = EaselStore::Get(profile);
  const sync_pb::CompassEasel* easel =
      store ? store->FindEasel(saved->easel_uuid()) : nullptr;
  if (!easel) {
    return;
  }
  if (new_easel) {
    PinEaselTab(BrowserOf(contents.get()), *easel);
  }
  ShowToast(contents.get(),
            l10n_util::GetStringFUTF8(IDS_COMPASS_EASEL_ADDED_TO,
                                      base::UTF8ToUTF16(EaselTitleOf(*easel))));
}

}  // namespace

// The overlay on one page, from start until the user picks or cancels.
class CaptureSession : public content::WebContentsObserver,
                       public content::WebContentsUserData<CaptureSession> {
 public:
  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;
  ~CaptureSession() override = default;

  // |screenshot|: Capture Screenshot's overlay (copy or save, or the whole
  // page) rather than Capture to Easel's.
  void Begin(bool screenshot) {
    poll_timer_.Stop();
    give_up_.Stop();
    screenshot_ = screenshot;
    base::ListValue boards;
    if (!screenshot) {
      Profile* profile =
          Profile::FromBrowserContext(GetWebContents().GetBrowserContext());
      for (const sync_pb::CompassEasel* easel : EaselsForPicker(profile)) {
        boards.Append(base::DictValue()
                          .Set("id", easel->uuid())
                          .Set("title", EaselTitleOf(*easel)));
      }
    }
    std::string json;
    base::JSONWriter::Write(boards, &json);
    // Keys go to the page with the overlay.
    GetWebContents().Focus();
    RunPageScript(
        &GetWebContents(),
        base::StrCat({"__compassEasel.start(", json,
                      screenshot ? ", {screenshot: true})" : ")"}),
        base::BindOnce(&CaptureSession::OnStarted, weak_factory_.GetWeakPtr()));
  }

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override {
    // The overlay went with the old page.
    End();
  }

 private:
  friend class content::WebContentsUserData<CaptureSession>;

  explicit CaptureSession(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<CaptureSession>(*contents) {}

  void OnStarted(base::Value started) {
    if (!started.is_bool() || !started.GetBool()) {
      ShowToast(&GetWebContents(), l10n_util::GetStringUTF8(
                                       IDS_COMPASS_EASEL_PAGE_NOT_CAPTURABLE));
      End();
      return;
    }
    poll_timer_.Start(FROM_HERE, kPollInterval,
                      base::BindRepeating(&CaptureSession::Poll,
                                          base::Unretained(this)));
    give_up_.Start(FROM_HERE, kGiveUpAfter,
                   base::BindOnce(&CaptureSession::GiveUp,
                                  base::Unretained(this)));
  }

  void Poll() {
    if (polling_) {
      return;
    }
    polling_ = true;
    RunPageScript(
        &GetWebContents(), "__compassEasel.poll()",
        base::BindOnce(&CaptureSession::OnPolled, weak_factory_.GetWeakPtr()),
        /*define=*/false);
  }

  void OnPolled(base::Value value) {
    polling_ = false;
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      // Still picking.
      return;
    }
    const std::string* state = dict->FindString("state");
    if (!state || *state != "picked") {
      // Cancelled, or the overlay is gone.
      End();
      return;
    }
    if (screenshot_) {
      OnScreenshotPicked(*dict, value);
      return;
    }
    const std::optional<PageView> view = PageViewFromValue(value);
    const std::string* easel = dict->FindString("easel");
    if (!view || !easel) {
      End();
      return;
    }
    CaptureRequest request;
    request.easel_uuid = *easel;
    request.region = ToRectF(dict->FindDouble("x").value_or(0),
                             dict->FindDouble("y").value_or(0),
                             dict->FindDouble("width").value_or(0),
                             dict->FindDouble("height").value_or(0));
    request.view = *view;
    const bool new_easel = request.easel_uuid.empty();
    content::WebContents* contents = &GetWebContents();
    // Done with the overlay; the copy needs only the page.
    End();
    AddCaptureToEasel(contents, std::move(request),
                      base::BindOnce(&OnAdded, contents->GetWeakPtr(),
                                     new_easel));
  }

  // Capture Screenshot: copy or save the region, or the whole page.
  void OnScreenshotPicked(const base::DictValue& dict,
                          const base::Value& value) {
    const std::string* action_name = dict.FindString("action");
    const std::optional<PageView> view = PageViewFromValue(value);
    if (!action_name || !view) {
      End();
      return;
    }
    const ScreenshotAction action = *action_name == "save"
                                        ? ScreenshotAction::kSave
                                        : ScreenshotAction::kCopy;
    content::WebContents* contents = &GetWebContents();
    base::WeakPtr<content::WebContents> weak = contents->GetWeakPtr();
    ScreenshotCallback done = base::BindOnce(
        [](base::WeakPtr<content::WebContents> contents,
           base::expected<std::string, std::string> result) {
          if (contents) {
            ShowToast(contents.get(),
                      result.has_value() ? *result : result.error());
          }
        },
        weak);
    const bool full = dict.FindBool("full").value_or(false);
    const gfx::RectF region = ToRectF(dict.FindDouble("x").value_or(0),
                                      dict.FindDouble("y").value_or(0),
                                      dict.FindDouble("width").value_or(0),
                                      dict.FindDouble("height").value_or(0));
    // Done with the overlay; the copy needs only the page.
    End();
    if (full) {
      CaptureFullPage(contents, action, std::move(done));
      return;
    }
    CopyPageRegion(
        contents, region, *view,
        base::BindOnce(
            [](base::WeakPtr<content::WebContents> contents,
               ScreenshotAction action, ScreenshotCallback done,
               base::expected<SkBitmap, std::string> bitmap) {
              if (!contents) {
                return;
              }
              if (!bitmap.has_value()) {
                std::move(done).Run(base::unexpected(bitmap.error()));
                return;
              }
              FinishScreenshot(contents.get(), action,
                               std::move(bitmap.value()), std::move(done));
            },
            weak, action, std::move(done)));
  }

  void GiveUp() {
    RunPageScript(&GetWebContents(), "__compassEasel.cancel()",
                  base::DoNothing(), /*define=*/false);
    End();
  }

  // Deletes this.
  void End() { GetWebContents().RemoveUserData(UserDataKey()); }

  base::RepeatingTimer poll_timer_;
  base::OneShotTimer give_up_;
  bool polling_ = false;
  bool screenshot_ = false;
  base::WeakPtrFactory<CaptureSession> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(CaptureSession);

bool CanCaptureToEasel(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  const GURL& url = contents->GetLastCommittedURL();
  if (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIsFile()) {
    return false;
  }
  return EaselStore::Get(
             Profile::FromBrowserContext(contents->GetBrowserContext())) !=
         nullptr;
}

void StartCaptureToEasel(content::WebContents* contents) {
  if (!CanCaptureToEasel(contents)) {
    return;
  }
  CaptureSession::CreateForWebContents(contents);
  CaptureSession::FromWebContents(contents)->Begin(/*screenshot=*/false);
}

bool CanCaptureScreenshot(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  const GURL& url = contents->GetLastCommittedURL();
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile();
}

void StartCaptureScreenshot(content::WebContents* contents) {
  if (!CanCaptureScreenshot(contents)) {
    return;
  }
  CaptureSession::CreateForWebContents(contents);
  CaptureSession::FromWebContents(contents)->Begin(/*screenshot=*/true);
}

SidebarNode* PinEaselTab(BrowserWindowInterface* browser,
                         const sync_pb::CompassEasel& easel) {
  WindowFeatures* features = browser ? WindowFeatures::From(browser) : nullptr;
  if (!features || features->is_parking_window() ||
      features->swapped_away()) {
    return nullptr;
  }
  SpaceModel& space = features->space_model();
  SidebarNode* parent =
      space.has_pinned() ? space.pinned_root() : space.today_root();
  if (!space.CanContain(parent, SidebarNode::Type::kTab)) {
    return nullptr;
  }
  // At the end of Pinned, as Pin tab puts entries; at the top of Today.
  const size_t index = space.has_pinned() ? parent->children().size() : 0;
  return space.AddTab(parent, index, GURL(EaselUrl(easel.uuid())),
                      EaselTabTitle(easel));
}

void NewEaselInWindow(BrowserWindowInterface* browser) {
  EaselStore* store = EaselStore::Get(browser->GetProfile());
  if (!store) {
    return;
  }
  // Right after startup the store may still be reading its file.
  store->WhenLoaded(base::BindOnce(
      [](base::WeakPtr<BrowserWindowInterface> browser,
         base::WeakPtr<EaselStore> store) {
        if (!browser || !store) {
          return;
        }
        base::expected<sync_pb::CompassEasel, std::string> easel =
            store->CreateEasel(NewEaselTitle());
        if (!easel.has_value()) {
          return;
        }
        SidebarNode* node = PinEaselTab(browser.get(), *easel);
        WindowFeatures* features = WindowFeatures::From(browser.get());
        if (!node || !features || !features->has_tab_strip_bridge()) {
          return;
        }
        if (tabs::TabInterface* tab =
                features->tab_strip_bridge().OpenNode(node)) {
          TabStripModel* strip = browser->GetTabStripModel();
          strip->ActivateTabAt(strip->GetIndexOfTab(tab));
        }
      },
      browser->GetWeakPtr(), store->GetWeakPtr()));
}

void AppendPageContextMenuItems(content::WebContents* contents,
                                ui::SimpleMenuModel* model) {
  const bool easel = CanCaptureToEasel(contents);
  const bool screenshot = CanCaptureScreenshot(contents);
  if (!easel && !screenshot) {
    return;
  }
  model->AddSeparator(ui::NORMAL_SEPARATOR);
  if (screenshot) {
    model->AddItem(
        kCaptureScreenshotContextCommand,
        l10n_util::GetStringUTF16(IDS_COMPASS_CONTEXT_MENU_CAPTURE_SCREENSHOT));
  }
  if (easel) {
    model->AddItem(
        kCaptureToEaselContextCommand,
        l10n_util::GetStringUTF16(IDS_COMPASS_CONTEXT_MENU_CAPTURE_TO_EASEL));
  }
}

bool IsCompassContextMenuCommand(int id) {
  return id == kCaptureToEaselContextCommand ||
         id == kCaptureScreenshotContextCommand;
}

bool ExecuteCompassContextMenuCommand(content::WebContents* contents,
                                      int id) {
  if (id == kCaptureToEaselContextCommand) {
    StartCaptureToEasel(contents);
    return true;
  }
  if (id == kCaptureScreenshotContextCommand) {
    StartCaptureScreenshot(contents);
    return true;
  }
  return false;
}

}  // namespace compass::easels
