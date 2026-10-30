// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/browser_view_exchange.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_active_state_manager/browser_active_state_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/immersive/immersive_mode_controller.h"
#include "chrome/browser/ui/views/color_provider_browser_helper.h"
#include "chrome/browser/ui/views/extensions/extension_keybinding_registry_views.h"
#include "chrome/browser/ui/views/frame/browser_frame_view.h"
#include "chrome/browser/ui/views/frame/browser_native_widget.h"
#include "chrome/browser/ui/views/frame/browser_root_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/infobars/infobar_view.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view_utils.h"
#include "ui/views/window/non_client_view.h"

namespace compass {

namespace {

// |widget|'s own child widgets, not theirs.
std::vector<views::Widget*> GetChildWidgets(views::Widget& widget) {
  std::vector<views::Widget*> children;
  for (views::Widget* child :
       views::Widget::GetAllChildWidgets(widget.GetNativeView())) {
    if (child != &widget && child->parent() == &widget) {
      children.push_back(child);
    }
  }
  return children;
}

}  // namespace

// static
void BrowserViewExchange::Exchange(BrowserView& a, BrowserView& b) {
  BrowserWidget* widget_a = a.browser_widget_.get();
  BrowserWidget* widget_b = b.browser_widget_.get();
  const bool a_was_active = widget_a->IsActive();
  const bool b_was_active = widget_b->IsActive();
  const bool a_was_last_active =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile() == a.browser();
  // Child widgets (bubbles, tab-modal dialogs, the find bar, the overlays)
  // belong to the view in their window: they move with it, so the leaving
  // view's go out of sight in the hidden window and close with it.
  const std::vector<views::Widget*> children_a = GetChildWidgets(*widget_a);
  const std::vector<views::Widget*> children_b = GetChildWidgets(*widget_b);
  Unbind(a);
  Unbind(b);

  // Pointers first: views ask for their frame view while being moved, and
  // the widgets make frame views for the view they're about to hold.
  std::swap(a.browser_widget_, b.browser_widget_);
  widget_a->browser_view_ = &b;
  widget_b->browser_view_ = &a;
  widget_a->root_view_->browser_view_ = &b;
  widget_b->root_view_->browser_view_ = &a;
  SetNativeWidgetView(*widget_a, &b);
  SetNativeWidgetView(*widget_b, &a);
  // Upstream finds a window's view, and its profile, through these.
  for (BrowserView* view : {&a, &b}) {
    view->browser_widget_->SetNativeWindowProperty(BrowserView::kBrowserViewKey,
                                                   view);
    view->browser_widget_->SetNativeWindowProperty(
        Profile::kProfileKey, view->browser()->GetProfile());
  }
  widget_a->ExchangeContentsWith(*widget_b);
  for (views::Widget* child : children_a) {
    views::Widget::ReparentNativeView(child->GetNativeView(),
                                      widget_b->GetNativeView());
    // Bubbles were for what the user saw (the command bar, the header's
    // menus and prompts): they close. Tab-modal dialogs (bubbles too) stay
    // with their tab.
    views::WidgetDelegate* delegate = child->widget_delegate();
    if (delegate && delegate->AsBubbleDialogDelegate() &&
        delegate->GetModalType() != ui::mojom::ModalType::kChild) {
      child->Hide();
      child->Close();
    }
  }
  for (views::Widget* child : children_b) {
    views::Widget::ReparentNativeView(child->GetNativeView(),
                                      widget_a->GetNativeView());
  }

  Rebind(b);
  Rebind(a);

  // Activation didn't change for the native windows, so their views hear it
  // here.
  if (a_was_active != b_was_active) {
    b.OnWidgetActivationChanged(widget_a, a_was_active);
    a.OnWidgetActivationChanged(widget_b, b_was_active);
  }
  // As BrowserView::Show() does: the window the user was in shows |b|'s
  // browser now (the last used profile, new windows from the Dock).
  if (a_was_last_active) {
    BrowserActiveStateManager::From(b.browser())->DidBecomeActive();
  }
}

// static
void BrowserViewExchange::Unbind(BrowserView& view) {
  if (views::FocusManager* focus_manager = view.GetFocusManager()) {
    focus_manager->UnregisterAccelerators(&view);
  }
  view.widget_observation_.Reset();
  view.focus_manager_observation_.Reset();
  view.paint_as_active_subscription_ = {};
  // Immersive fullscreen holds the native window and its focus manager; the
  // view that comes in turns it on again (Rebind()).
  if (auto* immersive = ImmersiveModeController::From(view.browser());
      immersive && immersive->IsEnabled()) {
    immersive->SetEnabled(false);
  }
}

// static
void BrowserViewExchange::Rebind(BrowserView& view) {
  BrowserWidget* widget = view.browser_widget_.get();
  views::NonClientView* non_client_view = widget->non_client_view();
  if (!widget->browser_native_widget_->UsesNativeSystemMenu()) {
    non_client_view->set_context_menu_controller(widget);
  }

  view.widget_observation_.Observe(widget);
  view.focus_manager_observation_.Observe(widget->GetFocusManager());
  // The browser's active state follows its native window
  // (BrowserView::set_browser_widget()).
  view.paint_as_active_subscription_ =
      widget->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
          &BrowserView::PaintAsActiveChanged, base::Unretained(&view)));
  view.LoadAccelerators();
  // Window features made for the first widget.
  if (auto* keybindings =
          ExtensionKeybindingRegistryViews::From(view.browser())) {
    keybindings->SetFocusManager(widget->GetFocusManager());
  }
  if (auto* colors = ColorProviderBrowserHelper::From(view.browser())) {
    colors->SetColorProviderSource(widget);
  }

  // Shown infobars track focus with the focus manager of the widget they
  // showed in, which closes after the exchange.
  if (views::View* infobars = view.infobar_container()) {
    for (views::View* child : infobars->children()) {
      if (auto* infobar = views::AsViewClass<InfoBarView>(child);
          infobar && infobar->owner()) {
        infobar->SetFocusManager(widget->GetFocusManager());
      }
    }
  }

  BrowserFrameView* frame_view = view.GetFrameView();
  frame_view->OnBrowserViewInitViewsComplete();
  frame_view->UpdateMinimumSize();
  view.using_native_frame_ = widget->ShouldUseNativeFrame();

  widget->UpdateWindowTitle();
  widget->UpdateWindowIcon();
  non_client_view->ResetWindowControls();
  widget->ThemeChanged();
  widget->GetRootView()->InvalidateLayout();
  widget->LayoutRootViewIfNecessary();
  // The window stays fullscreen; the view that comes in hears it as if it
  // had entered (its frame view turns immersive fullscreen on).
  if (widget->IsFullscreen()) {
    view.FullscreenStateChanging();
    view.FullscreenStateChanged();
  }
}

#if !BUILDFLAG(IS_MAC)
// static
void BrowserViewExchange::SetNativeWidgetView(BrowserWidget& widget,
                                              BrowserView* view) {}
#endif

}  // namespace compass
