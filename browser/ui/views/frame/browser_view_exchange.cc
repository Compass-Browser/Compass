// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/frame/browser_view_exchange.h"

#include <utility>

#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/color_provider_browser_helper.h"
#include "chrome/browser/ui/views/extensions/extension_keybinding_registry_views.h"
#include "chrome/browser/ui/views/frame/browser_frame_view.h"
#include "chrome/browser/ui/views/frame/browser_native_widget.h"
#include "chrome/browser/ui/views/frame/browser_root_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/infobars/infobar_view.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view_utils.h"
#include "ui/views/window/non_client_view.h"

namespace strata {

// static
void BrowserViewExchange::Exchange(BrowserView& a, BrowserView& b) {
  BrowserWidget* widget_a = a.browser_widget_.get();
  BrowserWidget* widget_b = b.browser_widget_.get();
  const bool a_was_active = widget_a->IsActive();
  const bool b_was_active = widget_b->IsActive();
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

  Rebind(b);
  Rebind(a);

  // Activation didn't change for the native windows, so their views hear it
  // here.
  if (a_was_active != b_was_active) {
    b.OnWidgetActivationChanged(widget_a, a_was_active);
    a.OnWidgetActivationChanged(widget_b, b_was_active);
  }
}

// static
void BrowserViewExchange::Unbind(BrowserView& view) {
  if (views::FocusManager* focus_manager = view.GetFocusManager()) {
    focus_manager->UnregisterAccelerators(&view);
  }
  view.widget_observation_.Reset();
  view.focus_manager_observation_.Reset();
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

  // Child widgets of the old native window.
  for (views::Widget* child :
       {view.overlay_widget_.get(), view.tab_overlay_widget_.get()}) {
    if (child) {
      views::Widget::ReparentNativeView(child->GetNativeView(),
                                        widget->GetNativeView());
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
  if (widget->IsFullscreen()) {
    view.FullscreenStateChanged();
  }
}

#if !BUILDFLAG(IS_MAC)
// static
void BrowserViewExchange::SetNativeWidgetView(BrowserWidget& widget,
                                              BrowserView* view) {}
#endif

}  // namespace strata
