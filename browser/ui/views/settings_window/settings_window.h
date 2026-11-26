// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SETTINGS_WINDOW_SETTINGS_WINDOW_H_
#define COMPASS_BROWSER_UI_VIEWS_SETTINGS_WINDOW_SETTINGS_WINDOW_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "content/public/browser/web_contents_delegate.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

class GURL;
class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace views {
class NativeWidget;
class WebView;
class WidgetDelegate;
}  // namespace views

namespace compass {

// The settings window's path for upstream's settings |sub_page| ("sync",
// "boosts?add=example.com", "content/siteDetails?site=..."): the window's
// pane and view for it. Every sub-page has one (the closest pane, or "" for
// the window as it was): upstream's settings page is never shown
// (docs/design/settings-window.md, "Phases"). Optional for the callers that
// check.
std::optional<std::string> SettingsWindowPathFor(std::string_view sub_page);

// The URL the window loads to show |path|.
GURL SettingsWindowURL(std::string_view path);

// Compass's settings: chrome://compass-settings in a window of its own, one
// per profile, with the panes as toolbar tabs across its top (the page's).
// Not a browser window: it isn't in the sidebar, the switcher, window sync or
// session restore. On macOS the page runs under the title bar, which is
// transparent, with the traffic lights over the page's toolbar; the toolbar's
// empty parts (app-region: drag) move the window. The window takes the
// height the shown pane needs (SetContentHeight()). Deletes itself when it
// closes. See docs/design/settings-window.md.
class SettingsWindow : public content::WebContentsDelegate,
                       public views::WidgetObserver,
                       public ProfileObserver {
 public:
  // The page's toolbar height, which the traffic lights center on (macOS).
  static constexpr int kToolbarHeight = 78;
  static constexpr int kWidth = 840;

  // Opens |profile|'s settings window at |path| (SettingsWindowPathFor()),
  // or brings it to the front and shows |path| there.
  static void Show(Profile* profile, std::string_view path);

  // The window showing |contents|, if any.
  static SettingsWindow* FromWebContents(content::WebContents* contents);
  // |profile|'s window, if open.
  static SettingsWindow* FromProfile(Profile* profile);

  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;
  ~SettingsWindow() override;

  // The page's side, which shows a path when the window is opened at one
  // while open (SettingsWindowHandler); null when the page goes.
  void SetShowPathCallback(
      base::RepeatingCallback<void(const std::string&)> callback);

  // Resizes the window to |height| of content (CSS pixels), within the
  // screen, keeping its top where it is.
  void SetContentHeight(int height);

  void Close();

  Profile* profile() { return profile_; }
  content::WebContents* web_contents() { return contents_.get(); }
  views::Widget* widget() { return widget_.get(); }

  // content::WebContentsDelegate:
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;
  void CloseContents(content::WebContents* source) override;
  void DraggableRegionsChanged(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) override;

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

  // Whether |point| (in the web view's coordinates) is on the page's
  // toolbar's empty parts, which drag the window.
  bool IsDraggableAt(const gfx::Point& point) const;
  // The same for |point| in the widget's coordinates.
  bool IsDraggableAtWidgetPoint(const gfx::Point& point) const;

 private:
  explicit SettingsWindow(Profile* profile);

  void Open(std::string_view path);
  // Takes the window out of the profile's slot, if it's there.
  void Forget();
  void ShowPath(std::string_view path);
  // Opens |url| in a tab of the profile's last active window (a new window
  // if none).
  void OpenInBrowser(const GURL& url, WindowOpenDisposition disposition);

  const raw_ptr<Profile> profile_;
  std::unique_ptr<content::WebContents> contents_;
  std::unique_ptr<views::WidgetDelegate> widget_delegate_;
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<views::WebView> web_view_ = nullptr;
  std::optional<SkRegion> draggable_region_;
  base::RepeatingCallback<void(const std::string&)> show_path_;
  views::UnhandledKeyboardEventHandler unhandled_keyboard_event_handler_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  base::WeakPtrFactory<SettingsWindow> weak_factory_{this};
};

#if BUILDFLAG(IS_MAC)
// The window's NSWindow: titled, closable and miniaturizable, with the page
// under a transparent title bar and the traffic lights centered on the page's
// toolbar (settings_window_mac.mm).
views::NativeWidget* CreateSettingsNativeWidget(views::Widget* widget);
// Animates the window to |bounds|, as macOS's own settings windows change
// height between panes.
void AnimateSettingsWindowBounds(views::Widget* widget,
                                 const gfx::Rect& bounds);
#endif

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SETTINGS_WINDOW_SETTINGS_WINDOW_H_
